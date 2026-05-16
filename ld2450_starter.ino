/* mmwave pax counter. (left->right is deboard, right->left is board) */

#include <Arduino.h>
#include <LD2450.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <time.h>

// wifi access
const char* ssid = "NETWORK";       // fill in
const char* password = "PASSWORD"; // fill in

// Web server
WebServer server(80);

// ESP32 Serial2
#define LD2450_RX 16
#define LD2450_TX 17

unsigned long lastCSVLog = 0;
const unsigned long CSV_LOG_INTERVAL = 30000;
//timezone for timestamps - EST (Maryland)
const char* tzInfo = "EST5EDT,M3.2.0/2,M11.1.0/2";

LD2450 ld2450;

int passengerCount = 0;
bool tracking = false;
int startX = 0;
int lastX = 0;

// Tracking window constraints
const int TRACK_MIN_X = -500;
const int TRACK_MAX_X =  500;

// Timer for periodic FOV print
unsigned long lastCheck = 0;

bool isInTrackingWindow(LD2450::RadarTarget t)
{
  return t.valid &&
         t.x >= TRACK_MIN_X && t.x <= TRACK_MAX_X;
}

String getTimestamp()
{
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo))
  {
    return String(millis());
  }

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

void logPassengerCount(int count)
{
  File file = LittleFS.open("/passengers.csv", "a");
  if(!file) {
    print("File open failed")
    return;
  }
  file.print(getTimestamp());
  file.print(",");
  file.println(count);
  file.close();
  Serial.println(count);
}

void setupCSV()
{
  if (!LittleFS.begin(true))
  {
    Serial.println("littlefs did not begin");
    return;
  }

  if (!LittleFS.exists("/passengers.csv"))
  {
    File file = LittleFS.open("/passengers.csv", "w");

    if (file)
    {
      file.close();
      Serial.println("created csv");
    }
    else
    {
      Serial.println("failed to create csv");
    }
  }
}

void setupWebServer()
{
  server.on("/", HTTP_GET, []()
  {
    server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Passenger Counter</title>
  <meta http-equiv="refresh" content="30">
  <style>
    body { font-family: Roboto, sans-serif; padding: 24px; background: #f5f5f5; }
    .card { background: white; padding: 20px; border-radius: 12px; max-width: 900px; }
    #current { font-size: 32px; font-weight: bold; margin: 16px 0; }
    button { padding: 10px 14px; margin-right: 8px; cursor: pointer; }
    canvas { width: 100%; max-height: 360px; margin-top: 20px; }
    table { width: 100%; border-collapse: collapse; margin-top: 20px; }
    td { border: 1px solid #ddd; padding: 10px; }
    tr:nth-child(even) { background: #f2f2f2; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Passenger Counter</h1>
    <div id="current">Current Passengers: 0</div>

    <div style="margin: 16px 0;">
      <button onclick="clearCSV()">Reset CSV</button>
      <button onclick="downloadCSV()">Download CSV</button>
    </div>

    <h2>Rider Count Trend</h2>
    <canvas id="countChart" width="800" height="350"></canvas>

    <h2>Last 15 Entries</h2>
    <table id="csvTable"></table>
  </div>

  <script>
    function drawChart(rows) {
      const canvas = document.getElementById('countChart');
      const ctx = canvas.getContext('2d');

      ctx.clearRect(0, 0, canvas.width, canvas.height);

      if (rows.length === 0) return;

      const padding = 50;
      const chartWidth = canvas.width - padding * 2;
      const chartHeight = canvas.height - padding * 2;

      const counts = rows.map(r => Number(r[1]));
      const maxCount = Math.max(...counts, 1);

      ctx.beginPath();
      ctx.moveTo(padding, padding);
      ctx.lineTo(padding, canvas.height - padding);
      ctx.lineTo(canvas.width - padding, canvas.height - padding);
      ctx.stroke();

      const barGap = 8;
      const barWidth = chartWidth / rows.length - barGap;

      rows.forEach((row, i) => {
        const count = Number(row[1]);
        const barHeight = (count / maxCount) * chartHeight;
        const x = padding + i * (chartWidth / rows.length) + barGap / 2;
        const y = canvas.height - padding - barHeight;

        ctx.fillRect(x, y, barWidth, barHeight);
        ctx.fillText(count, x + barWidth / 3, y - 5);
      });

      const first = counts[0];
      const last = counts[counts.length - 1];

      const x1 = padding + barWidth / 2;
      const y1 = canvas.height - padding - (first / maxCount) * chartHeight;

      const x2 = padding + chartWidth - barWidth / 2;
      const y2 = canvas.height - padding - (last / maxCount) * chartHeight;

      ctx.beginPath();
      ctx.moveTo(x1, y1);
      ctx.lineTo(x2, y2);
      ctx.lineWidth = 3;
      ctx.stroke();
      ctx.lineWidth = 1;
    }

    async function clearCSV() {
      if (!confirm("Reset CSV and passenger count?")) return;

      await fetch('/clear-csv');
      loadCSV();
    }

    function downloadCSV() {
      const link = document.createElement('a');
      link.href = '/download.csv';
      link.download = 'passengers.csv';
      link.click();
    }

    async function loadCSV() {
      const res = await fetch('/data.csv');
      const text = await res.text();

      const lines = text
        .trim()
        .split('\n')
        .filter(line => line.trim().length > 0);

      if (lines.length === 0) {
        document.getElementById('current').innerText = 'Current Passengers: 0';
        document.getElementById('csvTable').innerHTML = '';
        drawChart([]);
        return;
      }

      const rows = lines
        .map(line => line.split(','))
        .filter(row => row.length >= 2 && !isNaN(Number(row[1])));

      if (rows.length === 0) {
        document.getElementById('current').innerText = 'Current Passengers: 0';
        document.getElementById('csvTable').innerHTML = '';
        drawChart([]);
        return;
      }

      const lastFifteen = rows.slice(-15).reverse();
      const lastRow = rows[rows.length - 1];

      document.getElementById('current').innerText =
        'Current Passengers: ' + lastRow[1];

      drawChart(lastFifteen.slice().reverse());

      document.getElementById('csvTable').innerHTML =
        lastFifteen.map(row =>
          '<tr><td>' + row[0] + '</td><td>' + row[1] + '</td></tr>'
        ).join('');
    }

    loadCSV();
    setInterval(loadCSV, 30000);
  </script>
</body>
</html>
    )rawliteral");
  });

  server.on("/data.csv", HTTP_GET, []()
  {
    File file = LittleFS.open("/passengers.csv", "r");
    if(!file) {
      return;
    }
    server.streamFile(file, "text/csv");
    file.close();
  });

  server.on("/download.csv", HTTP_GET, []()
  {
    File file = LittleFS.open("/passengers.csv", "r");

    if (!file)
    {
      return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=passengers.csv");
    server.streamFile(file, "text/csv");
    file.close();
  });

  server.on("/clear-csv", HTTP_GET, []()
  {
    File file = LittleFS.open("/passengers.csv", "w");

    if (!file)
    {
      server.send(500, "text/plain", "CSV did not clear");
      return;
    }

    file.close();

    passengerCount = 0;
    tracking = false;
    startX = 0;
    lastX = 0;
    lastCSVLog = millis();

    logPassengerCount(passengerCount);

    Serial.println("CSV cleared");
    server.send(200, "text/plain", "CSV cleared");
  });

  server.begin();
  Serial.println("Web server started");
}


void setupWiFi()
{
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("WiFi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
}

void setupTime()
{
  configTime(0, 0, "pool.ntp.org");

  setenv("TZ", tzInfo, 1);
  tzset();

  struct tm timeinfo;

  Serial.print("Time connection loading...");

  unsigned long startAttempt = millis();

  while (!getLocalTime(&timeinfo) && millis() - startAttempt < 10000)
  {
    delay(1000);
    Serial.print(".");
  }

  Serial.println();

  if (getLocalTime(&timeinfo))
  {
    Serial.println("Time synchronized");
    Serial.println(getTimestamp());
  }
  else
  {
    Serial.println("No NTP connection");
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("SETUP_STARTED");

  setupWiFi();
  setupTime();
  setupCSV();
  setupWebServer();

  Serial2.begin(256000, SERIAL_8N1, LD2450_RX, LD2450_TX);
  delay(200);

  ld2450.begin(Serial2, false);

  if (!ld2450.waitForSensorMessage())
  {
    Serial.println("Sensor connected");
  }
  else
  {
    Serial.println("SENSOR TEST: GOT NO VALID SENSORDATA - PLEASE CHECK CONNECTION!");
  }

  Serial.println("SETUP_FINISHED");
}

void loop()
{
  server.handleClient();
  ld2450.read();

  if (millis() - lastCheck > 2000)
  {
    lastCheck = millis();

    bool personDetected = false;

    for (int i = 0; i < ld2450.getSensorSupportedTargetCount(); i++)
    {
      LD2450::RadarTarget t = ld2450.getTarget(i);

      if (t.valid)
      {
        personDetected = true;

        Serial.print("Person in FOV: X=");
        Serial.print(t.x);
        Serial.print(" mm, Y=");
        Serial.print(t.y);
        Serial.print(" mm, Speed=");
        Serial.print(t.speed);

        if (isInTrackingWindow(t))
        {
          Serial.println(" cm/s | IN TRACKING WINDOW");
        }
        else
        {
          Serial.println(" cm/s | OUTSIDE TRACKING WINDOW");
        }

        break;
      }
    }

    if (!personDetected)
    {
      Serial.println("No person detected");
    }
  }

  // Log current passenger count every 30 seconds, no matter what
  if (millis() - lastCSVLog >= CSV_LOG_INTERVAL)
  {
    lastCSVLog = millis();
    logPassengerCount(passengerCount);
  }

  // Basic directional counting using target 0 only
  const LD2450::RadarTarget t = ld2450.getTarget(0);

  if (isInTrackingWindow(t))
  {
    if (!tracking)
    {
      tracking = true;
      startX = t.x;

      Serial.print("Tracking started. startX = ");
      Serial.println(startX);
    }

    lastX = t.x;
  }
  else
  {
    if (tracking)
    {
      int delta = lastX - startX;

      Serial.print("Tracking ended. startX = ");
      Serial.print(startX);
      Serial.print(", lastX = ");
      Serial.print(lastX);
      Serial.print(", delta = ");
      Serial.println(delta);

      if (delta > -150)
      {
        passengerCount++;
        Serial.println("Boarded (+1)");
        logPassengerCount(passengerCount);
      }
      else if (delta < -150)
      {
        passengerCount--;

        if (passengerCount < 0)
        {
          passengerCount = 0;
        }

        Serial.println("Deboarded (-1)");
        logPassengerCount(passengerCount);
      }
      else
      {
        Serial.println("Movement too small, no count change");
      }

      Serial.print("Total passengers: ");
      Serial.println(passengerCount);
      Serial.println("----------------------");

      tracking = false;
    }
  }
}