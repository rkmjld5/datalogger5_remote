<?php
/*
 * Data Logger remote configuration for Render + TiDB Cloud.
 *
 * Database settings are read from Render environment variables.
 * A local .env file is supported for local development.
 *
 * IMPORTANT:
 * Never commit real database passwords to GitHub.
 */

date_default_timezone_set('Asia/Kolkata');

/* ---------------------------------------------------------
   1. Read a local .env file when available.
   Render environment variables take precedence.
   --------------------------------------------------------- */

$envFile = __DIR__ . '/.env';

if (is_file($envFile) && is_readable($envFile)) {

    $envLines = file(
        $envFile,
        FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES
    );

    if ($envLines !== false) {

        foreach ($envLines as $envLine) {

            $envLine = trim($envLine);

            if (
                $envLine === '' ||
                str_starts_with($envLine, '#') ||
                !str_contains($envLine, '=')
            ) {
                continue;
            }

            [$envKey, $envValue] = explode('=', $envLine, 2);

            $envKey = trim($envKey);
            $envValue = trim($envValue);

            if (strlen($envValue) >= 2) {

                $first = $envValue[0];
                $last = $envValue[strlen($envValue) - 1];

                if (
                    ($first === '"' && $last === '"') ||
                    ($first === "'" && $last === "'")
                ) {
                    $envValue = substr($envValue, 1, -1);
                }
            }

            // Do not override an existing process environment variable.
            if ($envKey !== '' && getenv($envKey) === false) {

                putenv($envKey . '=' . $envValue);

                $_ENV[$envKey] = $envValue;
            }
        }
    }
}

/* ---------------------------------------------------------
   2. Secure PHP session configuration.
   --------------------------------------------------------- */

if (session_status() === PHP_SESSION_NONE) {

    $httpsIsOn = (
        (
            !empty($_SERVER['HTTPS']) &&
            strtolower((string)$_SERVER['HTTPS']) !== 'off'
        ) ||
        (
            isset($_SERVER['HTTP_X_FORWARDED_PROTO']) &&
            strtolower(
                (string)$_SERVER['HTTP_X_FORWARDED_PROTO']
            ) === 'https'
        )
    );

    session_set_cookie_params([
        'httponly' => true,
        'secure' => $httpsIsOn,
        'samesite' => 'Lax'
    ]);

    session_start();
}

/* ---------------------------------------------------------
   3. Enable MySQLi error reporting.
   --------------------------------------------------------- */

mysqli_report(
    MYSQLI_REPORT_ERROR | MYSQLI_REPORT_STRICT
);

/* ---------------------------------------------------------
   4. Environment variable helper.
   --------------------------------------------------------- */

if (!function_exists('dataLoggerEnv')) {

    function dataLoggerEnv(
        string $key,
        string $default = ''
    ): string {

        $value = getenv($key);

        return $value === false
            ? $default
            : trim((string)$value);
    }
}

/* ---------------------------------------------------------
   5. Database configuration.
   --------------------------------------------------------- */

$servername = dataLoggerEnv('DB_HOST', 'localhost');

$dbUsername = dataLoggerEnv('DB_USER', 'root');

$dbPassword = dataLoggerEnv('DB_PASSWORD', '');

$dbname = dataLoggerEnv('DB_NAME', 'espdemo2');

$dbport = (int)dataLoggerEnv('DB_PORT', '3306');

$dbSslCa = dataLoggerEnv(
    'DB_SSL_CA',
    '/etc/ssl/certs/ca-certificates.crt'
);

/* ---------------------------------------------------------
   6. Connect to MySQL or TiDB Cloud.
   --------------------------------------------------------- */

try {

    if (
        $servername === '' ||
        $dbUsername === '' ||
        $dbname === '' ||
        $dbport < 1 ||
        $dbport > 65535
    ) {

        throw new RuntimeException(
            'Required database settings are missing or invalid.'
        );
    }

    $conn = mysqli_init();

    if (!$conn) {

        throw new RuntimeException(
            'Unable to initialize MySQL connection.'
        );
    }

    $conn->options(
        MYSQLI_OPT_CONNECT_TIMEOUT,
        15
    );

    $isRemoteDatabase = !in_array(
        strtolower($servername),
        ['localhost', '127.0.0.1', '::1'],
        true
    );

    if ($isRemoteDatabase) {

        /*
         * Remote TiDB Cloud connection.
         * TLS encryption is enabled.
         */

        if (defined('MYSQLI_OPT_SSL_VERIFY_SERVER_CERT')) {

            $conn->options(
                MYSQLI_OPT_SSL_VERIFY_SERVER_CERT,
                true
            );
        }

        $caFile = (
            $dbSslCa !== '' &&
            is_readable($dbSslCa)
        ) ? $dbSslCa : null;

        $conn->ssl_set(
            null,
            null,
            $caFile,
            null,
            null
        );

        $conn->real_connect(
            $servername,
            $dbUsername,
            $dbPassword,
            $dbname,
            $dbport,
            null,
            MYSQLI_CLIENT_SSL
        );

    } else {

        /*
         * Local XAMPP connection.
         */

        $conn->real_connect(
            $servername,
            $dbUsername,
            $dbPassword,
            $dbname,
            $dbport
        );
    }

    $conn->set_charset('utf8mb4');

} catch (Throwable $e) {

    error_log(
        'Data Logger database connection failed: ' .
        $e->getMessage()
    );

    http_response_code(500);

    exit(
        'Database connection failed. Please check the ' .
        'Render Environment settings and TiDB Cloud ' .
        'connection permissions.'
    );
}

/* ---------------------------------------------------------
   7. HTML output escaping helper.
   --------------------------------------------------------- */

if (!function_exists('h')) {

    function h($value): string {

        return htmlspecialchars(
            (string)($value ?? ''),
            ENT_QUOTES,
            'UTF-8'
        );
    }
}

?>