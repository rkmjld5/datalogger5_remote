<?php
/*
 * Data Logger Remote Configuration
 * Platform: Render
 * Database: TiDB Cloud
 * Database name: espdemo2
 *
 * IMPORTANT:
 * 1. No session is started in this file.
 * 2. No output is generated during successful initialization.
 * 3. Database credentials must be configured in Render Environment.
 * 4. Do not add spaces, HTML, or a UTF-8 BOM before <?php.
 * 5. Do not add a closing PHP tag at the end of this file.
 */

/* ---------------------------------------------------------
   1. Timezone
   --------------------------------------------------------- */

// Default timezone for PHP operations.
// Sensor installation-local time is calculated separately
// using the timezone supplied by the ESP32.

date_default_timezone_set('UTC');

/* ---------------------------------------------------------
   2. Optional local .env file
   --------------------------------------------------------- */

// Render environment variables take precedence over .env.

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

            // Remove matching surrounding quotation marks.
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

            // Never overwrite an existing environment variable.
            if (
                $envKey !== '' &&
                getenv($envKey) === false
            ) {

                putenv($envKey . '=' . $envValue);
                $_ENV[$envKey] = $envValue;
            }
        }
    }
}

/* ---------------------------------------------------------
   3. Environment variable helper
   --------------------------------------------------------- */

if (!function_exists('dataLoggerEnv')) {

    function dataLoggerEnv(
        string $key,
        string $default = ''
    ): string {

        $value = getenv($key);

        if ($value === false) {
            return $default;
        }

        return trim((string)$value);
    }
}

/* ---------------------------------------------------------
   4. MySQLi error reporting
   --------------------------------------------------------- */

mysqli_report(
    MYSQLI_REPORT_ERROR | MYSQLI_REPORT_STRICT
);

/* ---------------------------------------------------------
   5. TiDB Cloud database configuration
   --------------------------------------------------------- */

// Set these values in Render > Your Service > Environment.

$servername = dataLoggerEnv('DB_HOST');

$dbUsername = dataLoggerEnv('DB_USER');

$dbPassword = dataLoggerEnv('DB_PASSWORD');

$dbname = dataLoggerEnv('DB_NAME', 'espdemo2');

$dbport = (int)dataLoggerEnv('DB_PORT', '4000');

$dbSslCa = dataLoggerEnv(
    'DB_SSL_CA',
    '/etc/ssl/certs/ca-certificates.crt'
);

/* ---------------------------------------------------------
   6. Validate configuration
   --------------------------------------------------------- */

if (
    $servername === '' ||
    $dbUsername === '' ||
    $dbPassword === '' ||
    $dbname === '' ||
    $dbport < 1 ||
    $dbport > 65535
) {

    error_log(
        'Data Logger: Required database environment ' .
        'variables are missing or invalid.'
    );

    http_response_code(500);

    exit(
        'Database configuration error. ' .
        'Check Render Environment settings.'
    );
}

/* ---------------------------------------------------------
   7. Connect to the database
   --------------------------------------------------------- */

try {

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
         * TiDB Cloud connection.
         * TLS is required.
         */

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
        'Database connection failed. ' .
        'Check Render Environment settings, ' .
        'TiDB Cloud credentials, and TLS configuration.'
    );
}

/* ---------------------------------------------------------
   8. HTML output escaping helper
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

/*
 * IMPORTANT:
 * Do not start a PHP session in this file.
 * Do not echo, print, or output HTML here.
 * Do not add a closing PHP tag.
 */
