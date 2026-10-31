
<?php
/*
 * ESP32 Data Logger 4 Remote
 *
 * Endpoint: /save3_csv.php
 *
 * Live ESP32 sensor upload to TiDB Cloud.
 *
 * POST fields:
 * device_id, api_token, value1, value2, value3,
 * timestamp (Unix UTC), timezone
 *
 * Database: espdemo2
 * Table: logs
 *
 * Date and Time = installation-local time.
 * TimeStamp = UTC.
 */

// IMPORTANT:
// Load config.php BEFORE sending HTTP headers.
// config.php may configure and start a PHP session.

require_once __DIR__ . '/config.php';

header('Content-Type: application/json; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');

function saveRespond(int $status, array $data): void
{
    http_response_code($status);

    echo json_encode(
        $data,
        JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT
    );

    exit;
}

try {

    // 1. POST requests only.

    if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {

        header('Allow: POST');

        saveRespond(405, [
            'status' => 'error',
            'message' => 'POST request required.'
        ]);
    }

    // 2. Read credentials.

    $deviceID = trim((string)($_POST['device_id'] ?? ''));
    $apiToken = trim((string)($_POST['api_token'] ?? ''));

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

    // 3. Verify registered ESP device.

    $stmt = $conn->prepare(
        'SELECT token_hash, enabled
         FROM esp_devices
         WHERE device_id = ?
         LIMIT 1'
    );

    if (!$stmt) {
        throw new RuntimeException(
            'Could not prepare device authentication query.'
        );
    }

    $stmt->bind_param('s', $deviceID);

    if (!$stmt->execute()) {
        throw new RuntimeException(
            'Device authentication query failed.'
        );
    }

    $result = $stmt->get_result();
    $device = $result->fetch_assoc();

    $stmt->close();

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

    // 4. Read sensor values.

    $value1 = trim((string)($_POST['value1'] ?? ''));
    $value2 = trim((string)($_POST['value2'] ?? ''));
    $value3 = trim((string)($_POST['value3'] ?? ''));

    $timestampRaw = trim((string)($_POST['timestamp'] ?? ''));
    $timezone = trim((string)($_POST['timezone'] ?? ''));

    // 5. Validate sensor values.

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

    // 6. Validate Unix timestamp.

    if (
        $timestampRaw === '' ||
        !ctype_digit($timestampRaw)
    ) {
        saveRespond(400, [
            'status' => 'error',
            'message' => 'Invalid Unix timestamp.'
        ]);
    }

    $unixTimestamp = (int)$timestampRaw;

    if ($unixTimestamp <= 0) {
        saveRespond(400, [
            'status' => 'error',
            'message' => 'Timestamp must be positive.'
        ]);
    }

    // 7. Validate timezone.

    if (
        $timezone === '' ||
        strlen($timezone) > 64 ||
        !in_array(
            $timezone,
            DateTimeZone::listIdentifiers(DateTimeZone::ALL_WITH_BC),
            true
        )
    ) {
        saveRespond(400, [
            'status' => 'error',
            'message' => 'Invalid installation timezone.'
        ]);
    }

    $tz = new DateTimeZone($timezone);

    // 8. Convert Unix UTC timestamp to installation-local time.

    $localDateTime = (
        new DateTimeImmutable('@' . $unixTimestamp)
    )->setTimezone($tz);

    $date = $localDateTime->format('Y-m-d');
    $time = $localDateTime->format('H:i:s');

    // 9. Keep TimeStamp in UTC.

    $timestampUTC = gmdate(
        'Y-m-d H:i:s',
        $unixTimestamp
    );

    // 10. Insert record into TiDB Cloud.
    // Uses the $conn connection from config.php.
    // No localhost/root connection is used.

    $stmt = $conn->prepare(
        'INSERT INTO logs
            (value1, value2, value3, `Date`, `Time`,
             `TimeStamp`, timezone, device_id)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?)'
    );

    if (!$stmt) {
        throw new RuntimeException(
            'Could not prepare logs insertion.'
        );
    }

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

    if (!$stmt->execute()) {

        $error = $stmt->error;
        $stmt->close();

        error_log(
            'Live record insertion failed: ' . $error
        );

        saveRespond(500, [
            'status' => 'error',
            'message' => 'Database insertion failed.',
            'device_id' => $deviceID
        ]);
    }

    $databaseID = $stmt->insert_id;

    $stmt->close();

    // 11. Return success.

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

} catch (Throwable $e) {

    error_log(
        'datalogger4_remote save3_csv.php: ' .
        $e->getMessage()
    );

    saveRespond(500, [
        'status' => 'error',
        'message' => 'Server or database error. Check Render logs.'
    ]);
}
