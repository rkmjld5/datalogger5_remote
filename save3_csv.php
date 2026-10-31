<?php
/*
 * ESP32 Data Logger Remote
 *
 * Endpoint: /save3_csv.php
 *
 * Database: espdemo2
 * Table: logs
 *
 * POST fields:
 * device_id, api_token, value1, value2, value3,
 * timestamp (Unix UTC), timezone
 *
 * Date       = Installation-local date.
 * Time       = Installation-local time.
 * TimeStamp  = UTC timestamp.
 *
 * IMPORTANT:
 * This endpoint does not require a PHP session.
 * No output should appear before the JSON response.
 */

// ---------------------------------------------------------
// 1. Set HTTP headers before loading configuration.
// ---------------------------------------------------------

header('Content-Type: application/json; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');
header('X-Content-Type-Options: nosniff');

// ---------------------------------------------------------
// 2. JSON response function.
// ---------------------------------------------------------

function saveRespond(int $status, array $data): void
{
    http_response_code($status);

    echo json_encode(
        $data,
        JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT
    );

    exit;
}

// ---------------------------------------------------------
// 3. Load configuration and connect to TiDB Cloud.
// ---------------------------------------------------------

try {

    require_once __DIR__ . '/config.php';

} catch (Throwable $e) {

    error_log(
        'save3_csv.php configuration error: ' .
        $e->getMessage()
    );

    saveRespond(500, [
        'status' => 'error',
        'message' => 'Database configuration failed.'
    ]);
}

// ---------------------------------------------------------
// 4. Accept POST requests only.
// ---------------------------------------------------------

if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {

    header('Allow: POST');

    saveRespond(405, [
        'status' => 'error',
        'message' => 'POST request required.'
    ]);
}

// ---------------------------------------------------------
// 5. Read device credentials.
// ---------------------------------------------------------

$deviceID = trim(
    (string)($_POST['device_id'] ?? '')
);

$apiToken = trim(
    (string)($_POST['api_token'] ?? '')
);

if (
    $deviceID === '' ||
    $apiToken === '' ||
    !preg_match('/^[A-Za-z0-9_-]{1,32}$/', $deviceID)
) {

    saveRespond(400, [
        'status' => 'error',
        'message' => 'Missing or invalid device credentials.'
    ]);
}

// ---------------------------------------------------------
// 6. Authenticate the registered ESP32.
// ---------------------------------------------------------

try {

    $stmt = $conn->prepare(
        'SELECT token_hash, enabled
         FROM esp_devices
         WHERE device_id = ?
         LIMIT 1'
    );

    $stmt->bind_param('s', $deviceID);

    $stmt->execute();

    $result = $stmt->get_result();

    $device = $result->fetch_assoc();

    $stmt->close();

} catch (Throwable $e) {

    error_log(
        'save3_csv.php device authentication error: ' .
        $e->getMessage()
    );

    saveRespond(500, [
        'status' => 'error',
        'message' => 'Device authentication query failed.'
    ]);
}

$storedHash = is_array($device)
    ? (string)($device['token_hash'] ?? '')
    : '';

$submittedHash = hash('sha256', $apiToken);

if (
    !$device ||
    (int)($device['enabled'] ?? 0) !== 1 ||
    $storedHash === '' ||
    !hash_equals($storedHash, $submittedHash)
) {

    saveRespond(403, [
        'status' => 'error',
        'message' => 'Device is not authorized.'
    ]);
}

// ---------------------------------------------------------
// 7. Read sensor values.
// ---------------------------------------------------------

$value1 = trim(
    (string)($_POST['value1'] ?? '')
);

$value2 = trim(
    (string)($_POST['value2'] ?? '')
);

$value3 = trim(
    (string)($_POST['value3'] ?? '')
);

$timestampRaw = trim(
    (string)($_POST['timestamp'] ?? '')
);

$timezone = trim(
    (string)($_POST['timezone'] ?? '')
);

// ---------------------------------------------------------
// 8. Validate sensor values.
// ---------------------------------------------------------

if (
    $value1 === '' ||
    $value2 === '' ||
    $value3 === '' ||
    strlen($value1) > 30 ||
    strlen($value2) > 30 ||
    strlen($value3) > 50
) {

    saveRespond(400, [
        'status' => 'error',
        'message' => 'Missing sensor values or field length exceeded.'
    ]);
}

// ---------------------------------------------------------
// 9. Validate Unix timestamp.
// ---------------------------------------------------------

if (
    $timestampRaw === '' ||
    !ctype_digit($timestampRaw)
) {

    saveRespond(400, [
        'status' => 'error',
        'message' => 'Invalid Unix timestamp.'
    ]);
}

$unixTimestamp = filter_var(
    $timestampRaw,
    FILTER_VALIDATE_INT
);

if (
    $unixTimestamp === false ||
    $unixTimestamp <= 0
) {

    saveRespond(400, [
        'status' => 'error',
        'message' => 'Timestamp must be a positive Unix timestamp.'
    ]);
}

// ---------------------------------------------------------
// 10. Validate installation timezone.
// ---------------------------------------------------------

if (
    $timezone === '' ||
    strlen($timezone) > 64 ||
    !in_array(
        $timezone,
        DateTimeZone::listIdentifiers(
            DateTimeZone::ALL_WITH_BC
        ),
        true
    )
) {

    saveRespond(400, [
        'status' => 'error',
        'message' => 'Invalid installation timezone.'
    ]);
}

// ---------------------------------------------------------
// 11. Calculate installation-local date and time.
// ---------------------------------------------------------

try {

    $tz = new DateTimeZone($timezone);

    $localDateTime = (
        new DateTimeImmutable('@' . $unixTimestamp)
    )->setTimezone($tz);

    $date = $localDateTime->format('Y-m-d');

    $time = $localDateTime->format('H:i:s');

    // Always store TimeStamp in UTC.
    $timestampUTC = gmdate(
        'Y-m-d H:i:s',
        $unixTimestamp
    );

} catch (Throwable $e) {

    error_log(
        'save3_csv.php timestamp conversion error: ' .
        $e->getMessage()
    );

    saveRespond(400, [
        'status' => 'error',
        'message' => 'Could not convert the supplied timestamp.'
    ]);
}

// ---------------------------------------------------------
// 12. Insert the live sensor record into TiDB Cloud.
// ---------------------------------------------------------

try {

    /*
     * The id column is intentionally omitted.
     * The logs.id column must generate its own ID,
     * normally through AUTO_INCREMENT.
     */

    $stmt = $conn->prepare(
        'INSERT INTO logs
            (
                value1,
                value2,
                value3,
                `Date`,
                `Time`,
                `TimeStamp`,
                timezone,
                device_id
            )
         VALUES (?, ?, ?, ?, ?, ?, ?, ?)'
    );

    $stmt->bind_param(
        'ssssssss',
        $value1,
        $value2,
        $value3,
        $date,
        $time,
        $timestampUTC,
        $timezone,
        $deviceID
    );

    $stmt->execute();

    $databaseID = $stmt->insert_id;

    $stmt->close();

} catch (Throwable $e) {

    error_log(
        'save3_csv.php database insertion failed: ' .
        $e->getMessage()
    );

    saveRespond(500, [
        'status' => 'error',
        'message' => 'Database insertion failed. Check Render logs.',
        'device_id' => $deviceID,
        'database_saved' => false
    ]);
}

// ---------------------------------------------------------
// 13. Return a successful JSON response.
// ---------------------------------------------------------

saveRespond(200, [
    'status' => 'success',
    'message' => 'Live record inserted into espdemo2.logs.',
    'database_id' => $databaseID,
    'device_id' => $deviceID,
    'value1' => $value1,
    'value2' => $value2,
    'value3' => $value3,
    'Date' => $date,
    'Time' => $time,
    'TimeStamp' => $timestampUTC,
    'timezone' => $timezone,
    'database_saved' => true
]);

