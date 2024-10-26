#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30100_PulseOximeter.h"

#define SCREEN_WIDTH 128 // Chiều rộng OLED, tính bằng pixel
#define SCREEN_HEIGHT 64 // Chiều cao OLED, tính bằng pixel
#define REPORTING_PERIOD_MS 1000
#define BUTTON_PIN 0 // Chân GPIO 0 được sử dụng cho nút nhấn

#define ledPin 1
#define PWMChannel 0
#define resolution 8   // 0->255
#define frequency 1000 // 1m

// Tạo đối tượng OLED kết nối qua I2C
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // oled không có chân reset vật lý

// PulseOximeter để đo nhịp tim và SpO2
PulseOximeter pox;
uint32_t tsLastReport = 0;
volatile bool deviceOn = true;           // Trạng thái bật/tắt của OLED (volatile để sử dụng trong ISR)
volatile bool buttonState = HIGH;        // Trạng thái nút nhấn hiện tại
volatile bool lastButtonState = HIGH;    // Trạng thái nút nhấn trước đó
volatile bool buttonReleased = false;    // Biến theo dõi khi nút được nhả ra
unsigned long lastDebounceTime = 0;      // Biến chống rung
const unsigned long debounceDelay = 100; // Thời gian chống rung nút nhấn (200ms)

// Số lượng giá trị cần lưu để tính trung bình (hàng đợi tối đa 5 giá trị)
const int period = 5;
float heartRateQueue[period]; // Hàng đợi cho nhịp tim
float spO2Queue[period];      // Hàng đợi cho SpO2
int queueIndex = 0;           // Chỉ mục hiện tại trong hàng đợi
int queueSize = 0;            // Kích thước hiện tại của hàng đợi

// Callback khi phát hiện nhịp tim
void onBeatDetected()
{
  Serial.println("Beat!");
}

// Hàm ngắt khi nhấn hoặc nhả nút
void IRAM_ATTR handleButtonPress()
{
  if ((millis() - lastDebounceTime) > debounceDelay)
  {
    deviceOn = !deviceOn;
    lastDebounceTime = millis();
  }
}

void setup()
{
  Serial.begin(9600);

  // PWM
  ledcSetup(PWMChannel, frequency, resolution);
  ledcAttachPin(ledPin, PWMChannel);

  // Thiết lập chân nút nhấn (BUTTON_PIN) làm đầu vào với điện trở kéo lên nội bộ
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Gán ngắt cho chân GPIO nút nhấn
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, RISING);

  // Khởi tạo I2C với chân 2 là SCL và chân 3 là SDA
  Wire.begin(3, 2); // SDA là chân 3, SCL là chân 2 trên ESP32-C3

  // Khởi tạo màn hình OLED với địa chỉ I2C 0x3C
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println(F("Failed to start SSD1306 OLED"));
    while (1)
      ;
  }

  delay(1000);         // đợi 2 giây để khởi tạo
  oled.clearDisplay(); // xóa màn hình

  // Khởi tạo cảm biến nhịp tim MAX30100
  Serial.print("Initializing pulse oximeter...");
  if (!pox.begin())
  {
    Serial.println("FAILED");
    while (1)
      ;
  }
  else
  {
    Serial.println("SUCCESS");
  }

  // Đăng ký callback khi phát hiện nhịp tim
  pox.setOnBeatDetectedCallback(onBeatDetected);

  oled.setTextSize(1);
  oled.setTextColor(WHITE);
}

void loop()
{
  // Cập nhật dữ liệu từ cảm biến MAX30100
  pox.update();

  // Nếu thiết bị đang bật, hiển thị OLED
  if (deviceOn)
  {
    if (millis() - tsLastReport > REPORTING_PERIOD_MS)
    {
      oled.clearDisplay();

      // Lấy giá trị nhịp tim và SpO2 hiện tại
      float currentHeartRate = pox.getHeartRate();
      float currentSpO2 = pox.getSpO2();

      // Thêm giá trị mới vào hàng đợi
      heartRateQueue[queueIndex] = currentHeartRate;
      spO2Queue[queueIndex] = currentSpO2;

      // Tăng chỉ mục hàng đợi và cuộn vòng
      queueIndex = (queueIndex + 1) % period;

      // Cập nhật kích thước hàng đợi (không vượt quá "period")
      if (queueSize < period)
      {
        queueSize++;
      }

      // Tính trung bình nhịp tim và SpO2 từ hàng đợi
      float sumHeartRate = 0;
      float sumSpO2 = 0;
      for (int i = 0; i < queueSize; i++)
      {
        sumHeartRate += heartRateQueue[i];
        sumSpO2 += spO2Queue[i];
      }
      float averageHeartRate = sumHeartRate / queueSize;
      float averageSpO2 = sumSpO2 / queueSize;

      // Hiển thị nhịp tim và SpO2 hiện tại và trung bình lên màn hình OLED
      oled.setCursor(0, 0);
      oled.print("Heart rate: ");
      oled.print(currentHeartRate);
      oled.println(" bpm");

      oled.setCursor(0, 16);
      oled.print("SpO2: ");
      oled.print(currentSpO2);
      oled.println(" %");

      oled.setCursor(0, 32);
      oled.print("Avg HR: ");
      oled.print(averageHeartRate);
      oled.println(" bpm");

      oled.setCursor(0, 48);
      oled.print("Avg SpO2: ");
      oled.print(averageSpO2);
      oled.println(" %");

      oled.display(); // Cập nhật hiển thị trên OLED

      // In ra cổng Serial
      Serial.print("Heart rate:");
      Serial.print(currentHeartRate);
      Serial.print(" bpm / SpO2:");
      Serial.print(currentSpO2);
      Serial.println(" %");

      // Điều khiển LED dựa trên nhịp tim trung bình
      if (averageHeartRate < 30 || averageHeartRate > 240)
      {
        // Sáng LED khi nhịp tim trung bình nằm ngoài khoảng 30-240 bpm
        ledcWrite(PWMChannel, 255); // Sáng tối đa
      }
      else
      {
        // Tắt LED khi nhịp tim trung bình nằm trong khoảng 30-240 bpm
        ledcWrite(PWMChannel, 0); // Tắt LED
      }

      tsLastReport = millis();
    }
  }
  else
  {
    oled.clearDisplay();
    oled.display();
  }
}
