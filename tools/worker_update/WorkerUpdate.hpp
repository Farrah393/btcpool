/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#pragma once

#include <string>
#include <map>
#include <atomic>
#include <thread>

#include <glog/logging.h>

#include "Kafka.h"
#include "MySQLConnection.hpp"
#include "utilities_js.hpp"
#include "utils.hpp"

using std::string;

class WorkerUpdate {
public:
    WorkerUpdate(
        string consumeBrokers, string consumeTopic, string consumeGroupId,
        const MysqlConnectInfo &mysqlInfo
    )
        : running_(false), messageNumber_(0)
        , consumeBrokers_(consumeBrokers), consumeTopic_(consumeTopic), consumeGroupId_(consumeGroupId)
        , consumer_(consumeBrokers_.c_str(), consumeTopic_.c_str(), 0/* patition */, consumeGroupId_.c_str())
        , mysqlInfo_(mysqlInfo)
    {
    }

    bool init() {
        LOG(INFO) << "setup kafka consumer...";
        if (!consumer_.setup()) {
            LOG(ERROR) << "setup kafka consumer fail";
            return false;
        }
        
        mysqlConn_ = make_shared<MySQLConnection>(mysqlInfo_);
        if (!mysqlConn_->ping()) {
            LOG(INFO) << "common events db ping failure";
            return false;
        }

        return true;
    }

    void run() {
        const int32_t kTimeoutMs = 1000;
        running_ = true;

        LOG(INFO) << "waiting kafka messages...";
        while (running_) {
            //
            // consume message
            //
            rd_kafka_message_t *rkmessage;
            rkmessage = consumer_.consumer(kTimeoutMs);

            // timeout, most of time it's not nullptr and set an error:
            //          rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF
            if (rkmessage == nullptr) {
                continue;
            }

            // check error
            if (rkmessage->err) {
                if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                    // Reached the end of the topic+partition queue on the broker.
                    // Not really an error.
                    //      LOG(INFO) << "consumer reached end of " << rd_kafka_topic_name(rkmessage->rkt)
                    //      << "[" << rkmessage->partition << "] "
                    //      << " message queue at offset " << rkmessage->offset;
                    // acturlly
                    rd_kafka_message_destroy(rkmessage);  /* Return message to rdkafka */
                    continue;
                }

                LOG(ERROR) << "consume error for topic " << rd_kafka_topic_name(rkmessage->rkt)
                           << "[" << rkmessage->partition << "] offset " << rkmessage->offset
                           << ": " << rd_kafka_message_errstr(rkmessage);

                if (rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
                    rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC) {
                    LOG(FATAL) << "consume fatal";
                    running_ = false;
                    rd_kafka_message_destroy(rkmessage);  /* Return message to rdkafka */
                    continue;
                }

                rd_kafka_message_destroy(rkmessage);  /* Return message to rdkafka */
                continue;
            }
            
            DLOG(INFO) << "a new message, size: " << rkmessage->len;

            // repeat a message
            bool success = handleMessage(rkmessage);
            if (success) {
                messageNumber_++;
            }
            
            rd_kafka_message_destroy(rkmessage);  /* Return message to rdkafka */
        }

        LOG(INFO) << "kafka consumer stopped";
    }

    void stop() {
        LOG(INFO) << "stopping kafka consume...";
        running_ = false;
    }

    bool isRunning() {
        return running_;
    }

    size_t getMessageNumber() {
        return messageNumber_;
    }

    void resetMessageNumber() {
        messageNumber_ = 0;
    }

    void runMessageNumberDisplayThread(time_t interval) {
        std::thread t([this, interval] () {
            this->resetMessageNumber();
            while (this->isRunning()) {
                sleep(interval);
                size_t num = this->getMessageNumber();
                this->resetMessageNumber();
                displayMessageNumber(num, interval);
            }
        });
        t.detach();
    }

protected:
    bool handleMessage(rd_kafka_message_t *rkmessage) {
        // check error
        if (rkmessage->err) {
            if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
            // Reached the end of the topic+partition queue on the broker.
            // Not really an error.
            //      LOG(INFO) << "consumer reached end of " << rd_kafka_topic_name(rkmessage->rkt)
            //      << "[" << rkmessage->partition << "] "
            //      << " message queue at offset " << rkmessage->offset;
            // acturlly
            return false;
            }

            LOG(ERROR) << "consume error for topic " << rd_kafka_topic_name(rkmessage->rkt)
            << "[" << rkmessage->partition << "] offset " << rkmessage->offset
            << ": " << rd_kafka_message_errstr(rkmessage);

            if (rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
                rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC) {
            LOG(FATAL) << "consume fatal";
            }
            return false;
        }

        const char *message = (const char*)rkmessage->payload;
        DLOG(INFO) << "A New Common Event: " << string(message, rkmessage->len);

        JsonNode r;
        if (!JsonNode::parse(message, message + rkmessage->len, r)) {
            LOG(ERROR) << "decode common event failure";
            return false;
        }

        // check fields
        if (r["type"].type()    != Utilities::JS::type::Str ||
            r["content"].type() != Utilities::JS::type::Obj) {
            LOG(ERROR) << "common event missing some fields";
            return false;
        }

        // update worker status
        if (r["type"].str() == "worker_update") {
            // check fields
            if (r["content"]["user_id"].type()     != Utilities::JS::type::Int ||
                r["content"]["worker_id"].type()   != Utilities::JS::type::Int ||
                r["content"]["worker_name"].type() != Utilities::JS::type::Str ||
                r["content"]["miner_agent"].type() != Utilities::JS::type::Str) {
                LOG(ERROR) << "common event `worker_update` missing some fields";
                return false;
            }

            int32_t userId    = r["content"]["user_id"].int32();
            int64_t workerId  = r["content"]["worker_id"].int64();
            string workerName = filterWorkerName(r["content"]["worker_name"].str());
            string minerAgent = filterWorkerName(r["content"]["miner_agent"].str());

            updateWorkerStatusToDB(userId, workerId, workerName.c_str(), minerAgent.c_str());
            return true;
        }

        if (r["type"].str() == "miner_connect") {
            // check fields
            if (r["content"]["user_id"].type()     != Utilities::JS::type::Int ||
                r["content"]["worker_id"].type()   != Utilities::JS::type::Int ||
                r["content"]["worker_name"].type() != Utilities::JS::type::Str ||
                r["content"]["client_agent"].type() != Utilities::JS::type::Str) {
                LOG(ERROR) << "common event `miner_connect` missing some fields";
                return false;
            }

            int32_t userId    = r["content"]["user_id"].int32();
            int64_t workerId  = r["content"]["worker_id"].int64();
            string workerName = filterWorkerName(r["content"]["worker_name"].str());
            string minerAgent = filterWorkerName(r["content"]["client_agent"].str());

            updateWorkerStatusToDB(userId, workerId, workerName.c_str(), minerAgent.c_str());
            return true;
        }

        return false;
    }
    
    bool updateWorkerStatusToDB(
        const int32_t userId, const int64_t workerId,
        const char *workerName, const char *minerAgent) {
        string sql;
        char **row = nullptr;
        MySQLResult res;
        const string nowStr = date("%F %T", time(nullptr));

        // find the miner
        sql = StringFormat(
            "SELECT `group_id` FROM `mining_workers` "
            " WHERE `puid`=%d AND `worker_id`= %" PRId64"",
            userId, workerId);
        mysqlConn_->query(sql, res);

        if (res.numRows() > 0 && (row = res.nextRow()) != nullptr) {
            const int32_t groupId = atoi(row[0]);

            // group Id == 0: means the miner's status is 'deleted'
            // we need to move from 'deleted' group to 'default' group.
            sql = StringFormat(
                "UPDATE `mining_workers` SET `group_id`=%d, "
                " `worker_name`=\"%s\", `miner_agent`=\"%s\", "
                " `updated_at`=\"%s\" "
                " WHERE `puid`=%d AND `worker_id`= %" PRId64"",
                groupId == 0 ? userId * -1 : groupId,
                workerName, minerAgent,
                nowStr.c_str(),
                userId, workerId
            );
        }
        else {
            // we have to use 'ON DUPLICATE KEY UPDATE', because 'statshttpd' may insert
            // items to table.mining_workers between we 'select' and 'insert' gap.
            // 'statshttpd' will always set an empty 'worker_name'.
            sql = StringFormat(
                "INSERT INTO `mining_workers`(`puid`,`worker_id`,"
                " `group_id`,`worker_name`,`miner_agent`,"
                " `created_at`,`updated_at`) "
                " VALUES(%d,%" PRId64",%d,\"%s\",\"%s\",\"%s\",\"%s\")"
                " ON DUPLICATE KEY UPDATE "
                " `worker_name`= \"%s\",`miner_agent`=\"%s\",`updated_at`=\"%s\" ",
                userId, workerId,
                userId * -1,  // default group id
                workerName, minerAgent,
                nowStr.c_str(), nowStr.c_str(),
                workerName, minerAgent,
                nowStr.c_str());
        }

        if (mysqlConn_->execute(sql) == false) {
            LOG(ERROR) << "insert worker name failure";

            // try to reconnect mysql, so last update may success
            if (!mysqlConn_->reconnect()) {
                LOG(ERROR) << "updateWorkerStatusToDB: can't connect to pool DB";
            }

            return false;
        }

        return true;
    }

    void displayMessageNumber(size_t messageNumber, time_t time) {
        LOG(INFO) << "Handled " << messageNumber << " messages in " << time << " seconds";
    }

    std::atomic<bool> running_;
    size_t messageNumber_; // don't need thread safe (for logs only)

    string consumeBrokers_;
    string consumeTopic_;
    string consumeGroupId_;
    KafkaHighLevelConsumer consumer_;

    MysqlConnectInfo mysqlInfo_;
    shared_ptr<MySQLConnection> mysqlConn_;
};