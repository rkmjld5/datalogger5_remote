<?php
/*
 * Remote SD-card synchronization endpoint for Render + TiDB Cloud.
 * Required Render variable: API_TOKEN
 * Database connection is provided by config.php.
 */

require_once __DIR__ . '/config.php';

header('Content-Type: application/json; charset=utf-8');

$apiToken = dataLoggerEnv('ESP_API_TOKEN', ''); 

$csvFile = __DIR__ . DIRECTORY_SEPARATOR . 'sensor_data.csv';
$header = [
    'id', 'value1', 'value2', 'value3',
    'Date', 'Time', 'TimeStamp', 'timezone', 'device_id'
];

function syncRespond(int $code, array $body): void {
    http_response_code($code);
    echo json_encode($body, JSON_UNESCAPED_SLASHES);
    exit;
}

if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
    syncRespond(405, [
        'status' => 'error',
        'message' => 'POST request required.'
    ]);
}

if (
    $apiToken === '' ||
    !hash_equals($apiToken, (string)($_POST['api_token'] ?? ''))
) {
    syncRespond(403, [
        'status' => 'error',
        'message' => 'Invalid API token.'
    ]);
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

if (
    $csvId === '' || !ctype_digit($csvId) ||
    $deviceId === '' || $date === '' || $time === '' ||
    $timestamp === '' || $timezone === ''
) {
    syncRespond(400, [
        'status' => 'error',
        'message' => 'Missing or invalid CSV record fields.'
    ]);
}

if (
    strlen($deviceId) > 32 ||
    strlen($value1) > 30 ||
    strlen($value2) > 30 ||
    strlen($value3) > 50 ||
    strlen($timezone) > 64
) {
    syncRespond(400, [
        'status' => 'error',
        'message' => 'One or more fields exceed the logs-table field length.'
    ]);
}

if (
    !preg_match('/^\d{4}-\d{2}-\d{2}$/', $date) ||
    !preg_match('/^\d{2}:\d{2}:\d{2}$/', $time) ||
    !preg_match('/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/', $timestamp)
) {
    syncRespond(400, [
        'status' => 'error',
        'message' => 'Date, Time or TimeStamp format is invalid.'
    ]);
}

try {
    new DateTimeZone($timezone);
} catch (Throwable $e) {
    syncRespond(400, [
        'status' => 'error',
        'message' => 'Invalid timezone.'
    ]);
}

/*
 * TimeStamp represents UTC. Set the MySQL session timezone to UTC
 * before checking or inserting TIMESTAMP values.
 */
try {
    $conn->query("SET time_zone = '+00:00'");

    $check = $conn->prepare(
        'SELECT id FROM `logs`
         WHERE `device_id` = ? AND `TimeStamp` = ?
         LIMIT 1'
    );

    $check->bind_param('ss', $deviceId, $timestamp);
    $check->execute();
    $check->store_result();

    $existsInDb = ($check->num_rows > 0);
    $check->close();

    $dbInserted = false;
    $dbId = null;

    if (!$existsInDb) {
        $stmt = $conn->prepare(
            'INSERT INTO `logs`
            (`value1`,`value2`,`value3`,`Date`,`Time`,
             `TimeStamp`,`timezone`,`device_id`)
            VALUES (?,?,?,?,?,?,?,?)'
        );

        $stmt->bind_param(
            'ssssssss',
            $value1,
            $value2,
            $value3,
            $date,
            $time,
            $timestamp,
            $timezone,
            $deviceId
        );

        $stmt->execute();

        $dbInserted = true;
        $dbId = $stmt->insert_id;
        $stmt->close();
    }
} catch (Throwable $e) {
    error_log('SD-card sync database error: ' . $e->getMessage());

    syncRespond(500, [
        'status' => 'error',
        'message' => 'Database operation failed. Check Render and TiDB Cloud settings.'
    ]);
}

/* Open and lock the server CSV. */
$fp = @fopen($csvFile, 'c+');

if ($fp === false) {
    syncRespond(500, [
        'status' => 'error',
        'message' => 'Cannot create/open server sensor_data.csv. Check folder permissions.'
    ]);
}

if (!flock($fp, LOCK_EX)) {
    fclose($fp);

    syncRespond(500, [
        'status' => 'error',
        'message' => 'Could not lock server CSV file.'
    ]);
}

rewind($fp);
$first = fgetcsv($fp);
$csvExists = false;
$lastCsvId = 0;

if (
    $first === false ||
    (count($first) === 1 && trim((string)$first[0]) === '')
) {
    ftruncate($fp, 0);
    rewind($fp);

    if (fputcsv($fp, $header) === false) {
        flock($fp, LOCK_UN);
        fclose($fp);

        syncRespond(500, [
            'status' => 'error',
            'message' => 'Could not write the CSV header.'
        ]);
    }
} else {
    $normalized = array_map(
        static fn($x) => strtolower(trim((string)$x)),
        $first
    );

    if ($normalized !== array_map('strtolower', $header)) {
        flock($fp, LOCK_UN);
        fclose($fp);

        syncRespond(409, [
            'status' => 'error',
            'message' => 'Server sensor_data.csv header is not in the required logs-table order.',
            'required_header' => $header
        ]);
    }

    while (($row = fgetcsv($fp)) !== false) {
        if (isset($row[0]) && is_numeric($row[0])) {
            $lastCsvId = max($lastCsvId, (int)$row[0]);
        }

        if (
            count($row) >= 9 &&
            (string)$row[7] === $timezone &&
            (string)$row[8] === $deviceId &&
            (string)$row[6] === $timestamp
        ) {
            $csvExists = true;
        }
    }
}

$csvAppended = false;
$serverCsvId = null;

if (!$csvExists) {
    $serverCsvId = $lastCsvId + 1;

    fseek($fp, 0, SEEK_END);

    if (fputcsv($fp, [
        $serverCsvId,
        $value1,
        $value2,
        $value3,
        $date,
        $time,
        $timestamp,
        $timezone,
        $deviceId
    ]) === false) {
        flock($fp, LOCK_UN);
        fclose($fp);

        syncRespond(500, [
            'status' => 'error',
            'message' => 'Could not append record to server sensor_data.csv.'
        ]);
    }

    fflush($fp);
    $csvAppended = true;
}

flock($fp, LOCK_UN);
fclose($fp);

syncRespond(200, [
    'status' => 'success',
    'message' => 'SD card record synchronized; duplicates were skipped.',
    'device_id' => $deviceId,
    'timestamp' => $timestamp,
    'csv_appended' => $csvAppended,
    'server_csv_id' => $serverCsvId,
    'database_inserted' => $dbInserted,
    'mysql_id' => $dbId,
    'already_in_database' => $existsInDb
]);
?>
