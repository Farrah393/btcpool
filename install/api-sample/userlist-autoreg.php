<?php
# A demo for `users.list_id_api_url` in `sserver.cfg` with automatic registration enabled.
# It needs to be used with `autoreg.php`.
# Users in `userlist.json` will be read. This file is generated by `autoreg.php`.

header('Content-Type: application/json');

$last_id = (int) $_GET['last_id'];

$users = json_decode(file_get_contents(__DIR__.'/userlist.json'), true);

$requestedUsers = [];
foreach ($users as $name=>$id) {
    if ($id > $last_id) {
        $requestedUsers [$name] = $id;
    }
}

echo json_encode(
    [
        'err_no' => 0,
        'err_msg' => null,
        'data' => (object) $requestedUsers,
    ]
);