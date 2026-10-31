<?php
// Receives one row from ESP8266 LittleFS sensor_data.csv and merges it into
// the server-side sensor_data.csv and espdemo2.logs without duplicating records.
// Install beside save_csv.php in C:\xampp\htdocs\datalogger7_local\

require_once __DIR__ . '/config.php';
header('Content-Type: application/json; charset=utf-8');



$csvFile = __DIR__ . DIRECTORY_SEPARATOR . 'sensor_data.csv';
$header = ['id','value1','value2','value3','Date','Time','TimeStamp','timezone','device_id'];

function respond(int $code, array $body): void {
    http_response_code($code);
    echo json_encode($body, JSON_UNESCAPED_SLASHES);
    exit;
}

if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
    respond(405, ['status'=>'error','message'=>'POST request required.']);
}
if (!hash_equals(API_TOKEN, (string)($_POST['api_token'] ?? ''))) {
    respond(403, ['status'=>'error','message'=>'Invalid API token.']);
}

$csvId = trim((string)($_POST['csv_id'] ?? ''));
$value1 = trim((string)($_POST['value1'] ?? ''));
$value2 = trim((string)($_POST['value2'] ?? ''));
$value3 = trim((string)($_POST['value3'] ?? ''));
$date = trim((string)($_POST['Date'] ?? ''));
$time = trim((string)($_POST['Time'] ?? ''));
$timestamp = trim((string)($_POST['TimeStamp'] ?? ''));
$timezone = trim((string)($_POST['timezone'] ?? ''));
$deviceId = trim((string)($_POST['device_id'] ?? ''));

if ($csvId === '' || !ctype_digit($csvId) || $deviceId === '' || $date === '' ||
    $time === '' || $timestamp === '' || $timezone === '') {
    respond(400, ['status'=>'error','message'=>'Missing or invalid CSV record fields.']);
}
if (strlen($deviceId)>32 || strlen($value1)>30 || strlen($value2)>30 ||
    strlen($value3)>50 || strlen($timezone)>64) {
    respond(400, ['status'=>'error','message'=>'One or more fields exceed the logs-table field length.']);
}
if (!preg_match('/^\d{4}-\d{2}-\d{2}$/', $date) ||
    !preg_match('/^\d{2}:\d{2}:\d{2}$/', $time) ||
    !preg_match('/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/', $timestamp)) {
    respond(400, ['status'=>'error','message'=>'Date, Time or TimeStamp format is invalid.']);
}

// UTC is used for MySQL TIMESTAMP comparisons/inserts.
$db = @new mysqli(MYSQL_HOST, MYSQL_USER, MYSQL_PASSWORD, MYSQL_DATABASE);
if ($db->connect_errno) {
    respond(500, ['status'=>'error','message'=>'MySQL connection failed: '.$db->connect_error]);
}
$db->set_charset('utf8mb4');
if (!$db->query("SET time_zone = '+00:00'")) {
    $err = $db->error; $db->close();
    respond(500, ['status'=>'error','message'=>'Could not set MySQL UTC timezone: '.$err]);
}

// Detect whether this device/timestamp already exists in logs.
$existsInDb = false;
$check = $db->prepare('SELECT id FROM `logs` WHERE `device_id`=? AND `TimeStamp`=? LIMIT 1');
if (!$check) {
    $err=$db->error; $db->close();
    respond(500, ['status'=>'error','message'=>'Could not check logs for duplicate: '.$err]);
}
$check->bind_param('ss', $deviceId, $timestamp);
if (!$check->execute()) {
    $err=$check->error; $check->close(); $db->close();
    respond(500, ['status'=>'error','message'=>'Duplicate check failed: '.$err]);
}
$check->store_result();
$existsInDb = ($check->num_rows > 0);
$check->close();

$dbInserted = false;
$dbId = null;
if (!$existsInDb) {
    $stmt = $db->prepare('INSERT INTO `logs` (`value1`,`value2`,`value3`,`Date`,`Time`,`TimeStamp`,`timezone`,`device_id`) VALUES (?,?,?,?,?,?,?,?)');
    if (!$stmt) {
        $err=$db->error; $db->close();
        respond(500, ['status'=>'error','message'=>'Could not prepare logs insert: '.$err]);
    }
    $stmt->bind_param('ssssssss', $value1,$value2,$value3,$date,$time,$timestamp,$timezone,$deviceId);
    if (!$stmt->execute()) {
        $err=$stmt->error; $stmt->close(); $db->close();
        respond(500, ['status'=>'error','message'=>'Could not insert into logs: '.$err]);
    }
    $dbInserted = true;
    $dbId = $stmt->insert_id;
    $stmt->close();
}

// Open server CSV and ensure canonical header. The endpoint never migrates an
// unexpected file automatically; this protects existing data from accidental loss.
$fp = @fopen($csvFile, 'c+');
if ($fp === false) {
    $db->close();
    respond(500, ['status'=>'error','message'=>'Cannot create/open server sensor_data.csv. Check folder permissions.']);
}
if (!flock($fp, LOCK_EX)) {
    fclose($fp); $db->close();
    respond(500, ['status'=>'error','message'=>'Could not lock server CSV file.']);
}
rewind($fp);
$first = fgetcsv($fp);
$csvExists = false;
$lastCsvId = 0;
if ($first === false || (count($first)===1 && trim((string)$first[0])==='')) {
    ftruncate($fp,0); rewind($fp);
    fputcsv($fp,$header);
} else {
    $normalized = array_map(static fn($x)=>strtolower(trim((string)$x)), $first);
    if ($normalized !== array_map('strtolower',$header)) {
        flock($fp,LOCK_UN); fclose($fp); $db->close();
        respond(409, ['status'=>'error','message'=>'Server sensor_data.csv header is not in the required logs-table order.']);
    }
    while (($row=fgetcsv($fp)) !== false) {
        if (isset($row[0]) && is_numeric($row[0])) $lastCsvId=max($lastCsvId,(int)$row[0]);
        if (count($row)>=9 && (string)$row[7]===$timezone && (string)$row[8]===$deviceId &&
            (string)$row[6]===$timestamp) {
            $csvExists=true;
        }
    }
}
$csvAppended = false;
$serverCsvId = null;
if (!$csvExists) {
    $serverCsvId = $lastCsvId + 1;
    fseek($fp,0,SEEK_END);
    if (fputcsv($fp,[$serverCsvId,$value1,$value2,$value3,$date,$time,$timestamp,$timezone,$deviceId])===false) {
        flock($fp,LOCK_UN); fclose($fp); $db->close();
        respond(500, ['status'=>'error','message'=>'Could not append record to server sensor_data.csv.']);
    }
    fflush($fp);
    $csvAppended=true;
}
flock($fp,LOCK_UN);
fclose($fp);
$db->close();

respond(200, [
    'status'=>'success',
    'message'=>'LittleFS record synchronized; duplicates were skipped.',
    'device_id'=>$deviceId,
    'timestamp'=>$timestamp,
    'csv_appended'=>$csvAppended,
    'server_csv_id'=>$serverCsvId,
    'database_inserted'=>$dbInserted,
    'mysql_id'=>$dbId,
    'already_in_database'=>$existsInDb
]);
?>
