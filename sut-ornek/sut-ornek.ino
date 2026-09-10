/*
 * ================================================================
 *  Deneyap Kart - SUT Testi (tam yol self-test + 5 byte komut duzeltmesi)
 * ================================================================
 *  ONEMLI DUZELTME: Dokumanda gelen komut paketi 5 byte'tir:
 *     AA cmd cs 0D 0A          (Tablo 1)
 *  Giden durum paketi ise 6 byte'tir:
 *     AA Data1 Data2 cs 0D 0A  (Tablo 6)
 *  Onceki surumler gelen komutu da 6 byte bekliyordu; bu yuzden
 *  "SUT Baslat" hic islenmiyordu. Simdi ayristirici 0xAA icin 5 bekler.
 *
 *  SELF TEST tam yolu calistirir:
 *    komut paketi kur -> ayristir -> SUT aktif
 *    profil uret -> 36 byte pakete diz -> ayristir -> handleData -> algoritma
 *    Durdur paketi kur -> ayristir -> her sey sifirlanir
 *  Yani float<->byte, checksum, footer, ayiklama dahil hepsi test edilir.
 *
 *  SELF_TEST 1 -> kendi kendine test (Arduino seri monitor, 115200)
 *  SELF_TEST 0 -> gercek SUT: veri disaridan gelir (saha / resmi yazilim)
 * ================================================================
 */

#include <Arduino.h>

#define SELF_TEST           1     // <-- 1: kendi kendine test


/* ---------------- PİNLER ---------------- */
#define TEST_RX_PIN  D12
#define TEST_TX_PIN  D13
#define PYRO1_PIN    D4           // drogue
#define PYRO2_PIN    D5           // ana parasut



/* ---------------- PROTOKOL ---------------- */
#define HDR_CMD    0xAA
#define HDR_DATA   0xAB
#define CMD_SUT    0x22
#define CMD_STOP   0x24
#define FOOT1      0x0D
#define FOOT2      0x0A
#define CMDIN_LEN  5              // GELEN komut: AA cmd cs 0D 0A
#define STATUS_LEN 6              // GIDEN durum: AA D1 D2 cs 0D 0A
#define DATA_LEN   36             // GELEN veri : AB + 32 + cs + 0D 0A

/* ---------------- EŞİKLER (roketine gore ayarla) ---------------- */
const float    LAUNCH_ACC   = 30.0f;   // m/s²  kalkis ivmesi
const uint32_t BURNOUT_MS   = 3000;    // ms    yanma onlem suresi
const float    MIN_ARM_ALT  = 300.0f;  // m     bu kadar yukselmeden ayirma yok
const float    MAX_TILT     = 70.0f;   // °     dikeyden yatma siniri
const float    MAIN_ALT     = 500.0f;  // m     ana parasut irtifasi (yere gore)
const int      VOTE_N       = 5;       //       ardisik ornek (5 = 0.5 sn)
const float    ALT_DEADBAND = 1.0f;    // m     gurultu payi
const float    PRS_DEADBAND = 0.2f;    // mBar  gurultu payi

/* ---------------- VERİ VE DURUM ---------------- */
struct SensorData{
  float altitude,pressure;
  float accX,accY,accZ;
  float angX,angY,angZ;
};
SensorData sd;

bool     sutActive = false, sutPending = false;
uint32_t sutPendingAt = 0;
uint16_t statusBits = 0;

bool     launched  = false;
uint32_t launchMs  = 0;
float    launchAlt = 0, maxAlt = 0, minPrs = 1e9f;
int      altVote = 0, prsVote = 0;

const char *BIT_NAMES[8] = {
  "KALKIS ALGILANDI",
  "YANMA SURESI DOLDU",
  "MIN IRTIFA ESIGI ASILDI",
  "GOVDE ACISI SINIRI",
  "IRTIFA ALCALIYOR (tepe)",
  "DROGUE PARASUT EMRI",
  "BELIRLENEN IRTIFANIN ALTI",
  "ANA PARASUT EMRI"
};

/* ================================================================
 *  YARDIMCILAR
 * ================================================================ */
uint8_t checksum(const uint8_t *b, size_t n) {
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++) s += b[i];
  return (uint8_t)(s & 0xFF);
}

float getFloat(const uint8_t *src) { float v; memcpy(&v, src, 4); return v; }

void putFloat(uint8_t *dst, float v) {
  v = roundf(v * 100.0f) / 100.0f;       // virgulden sonra 2 basamak
  memcpy(dst, &v, 4);
}

void resetAll() {
  statusBits = 0;
  launched = false; launchMs = 0; launchAlt = 0;
  maxAlt = 0; minPrs = 1e9f; altVote = 0; prsVote = 0;
  digitalWrite(PYRO1_PIN, LOW);
  digitalWrite(PYRO2_PIN, LOW);
}

/* ================================================================
 *  UÇUŞ ALGORİTMASI (irtifa + basinc + yatma oylamasi)
 * ================================================================ */
void flightAlgorithm(const SensorData &d) {
  uint32_t now = millis();
  float acc = sqrtf(d.accX*d.accX + d.accY*d.accY + d.accZ*d.accZ);

  if (!launched && acc > LAUNCH_ACC) {              // Bit 0: kalkis
    launched = true; launchMs = now; launchAlt = d.altitude;
    maxAlt = d.altitude; minPrs = d.pressure;
    statusBits |= (1 << 0);
  }
  if (!launched) return;

  float agl = d.altitude - launchAlt;

  if (now - launchMs > BURNOUT_MS) statusBits |= (1 << 1);   // Bit 1
  if (agl > MIN_ARM_ALT)           statusBits |= (1 << 2);   // Bit 2

  float tilt = max(fabsf(d.angY), fabsf(d.angZ));            // Bit 3: yatma
  bool  tilted = tilt > MAX_TILT;
  if (tilted) statusBits |= (1 << 3);

  /* Tanik 1: irtifa - yeni maksimum oy sifirlar, dusus oy verir */
  if (d.altitude > maxAlt) { maxAlt = d.altitude; altVote = 0; }
  else if (maxAlt - d.altitude > ALT_DEADBAND) altVote++;

  /* Tanik 2: basinc - inerken artar */
  if (d.pressure < minPrs) { minPrs = d.pressure; prsVote = 0; }
  else if (d.pressure - minPrs > PRS_DEADBAND) prsVote++;

  bool altSaysDown = altVote >= VOTE_N;
  bool prsSaysDown = prsVote >= VOTE_N;

  /* Karar: (irtifa VE basinc) veya (yatik VE en az biri) */
  if ((altSaysDown && prsSaysDown) ||
      (tilted && (altSaysDown || prsSaysDown)))
    statusBits |= (1 << 4);                                  // Bit 4: tepe

  bool armed = (statusBits & (1 << 1)) && (statusBits & (1 << 2));
  if (armed && (statusBits & (1 << 4)) && !(statusBits & (1 << 5))) {
    statusBits |= (1 << 5);                                  // Bit 5: drogue
    digitalWrite(PYRO1_PIN, HIGH);
  }

  if ((statusBits & (1 << 5)) && agl < MAIN_ALT) {
    statusBits |= (1 << 6);                                  // Bit 6
    if (!(statusBits & (1 << 7))) {
      statusBits |= (1 << 7);                                // Bit 7: ana parasut
      digitalWrite(PYRO2_PIN, HIGH);
    }
  }
}

/* ================================================================
 *  GELEN PAKETLER (self-testte de dis dunyada da AYNI yol)
 * ================================================================ */
void handleCommand(const uint8_t *p) {             // 5 byte: AA cmd cs 0D 0A
  if (p[3] != FOOT1 || p[4] != FOOT2) return;
  
  if (p[1] == CMD_SUT)       { sutPending = true; sutPendingAt = millis(); }
  else if (p[1] == CMD_STOP) { sutActive = false; sutPending = false; resetAll(); }
}

void handleData(const uint8_t *p) {                // 36 byte
  if (!sutActive) return;                          // SUT disinda gormezden gel
  if (p[34] != FOOT1 || p[35] != FOOT2) return;
  if (p[33] != checksum(&p[1], 32)) return;        // bozuk paket, atla

  sd.altitude = getFloat(&p[1]);   sd.pressure = getFloat(&p[5]);
  sd.accX = getFloat(&p[9]);       sd.accY = getFloat(&p[13]);  sd.accZ = getFloat(&p[17]);
  sd.angX = getFloat(&p[21]);      sd.angY = getFloat(&p[25]);  sd.angZ = getFloat(&p[29]);

  flightAlgorithm(sd);
}

/* Byte byte ayristirici. Kaynaktan bagimsiz: seri port da self-test de
 * bu fonksiyonu besler, boylece tek bir yol test edilir. */
void feedByte(uint8_t b) {
  static uint8_t buf[DATA_LEN];
  static size_t  idx = 0, need = 0;

  if (idx == 0) {
    if      (b == HDR_CMD)  need = CMDIN_LEN;      // 0xAA -> 5 byte komut
    else if (b == HDR_DATA) need = DATA_LEN;       // 0xAB -> 36 byte veri
    else return;
  }
  buf[idx++] = b;
  if (idx == need) {
    if (buf[0] == HDR_CMD) handleCommand(buf); else handleData(buf);
    idx = 0;
  }
}

void pollTestPort() {
  while (TestPort.available()) feedByte(TestPort.read());
}

/* ================================================================
 *  GİDEN DURUM PAKETİ (6 byte)
 * ================================================================ */
void sendStatus(uint16_t bits) {
  uint8_t p[STATUS_LEN];
  p[0] = HDR_CMD;
  p[1] = (uint8_t)(bits & 0xFF);       // Data1 = bit 0-7
  p[2] = (uint8_t)(bits >> 8);         // Data2 = bit 8-15
  p[3] = checksum(&p[1], 2);
  p[4] = FOOT1; p[5] = FOOT2;
  TestPort.write(p, STATUS_LEN);
}

/* ================================================================
 *  SELF TEST
 * ================================================================ */
#if SELF_TEST

/* Sentetik ucus profili:
 * 0-3 sn   motor yanisi (60 m/s², dik)
 * 3-20 sn  suzulus, ~20. sn tepe (~1500 m), gitgide yatiyor
 * 20+ sn   inis (25 m/s), yatik */
void syntheticProfile(float t, SensorData &d) {
  const float ground = 1000.0f;
  float tilt;
  if (t < 3.0f) {
    d.altitude = ground + 25.0f * t * t;
    d.accX = 60.0f;
    tilt = 3.0f;
  } else if (t < 20.0f) {
    float u = t - 3.0f;
    d.altitude = ground + 225.0f + 150.0f*u - 4.4f*u*u;
    d.accX = -9.81f;
    tilt = 3.0f + u * 4.0f;
  } else {
    float peak = ground + 225.0f + 150.0f*17.0f - 4.4f*17.0f*17.0f;
    d.altitude = max(ground, peak - 25.0f*(t - 20.0f));
    d.accX = 5.0f;
    tilt = 85.0f;
  }
  d.pressure = 1013.25f * expf(-d.altitude / 8400.0f);
  d.accY = 0.0f; d.accZ = 0.0f;
  d.angX = 2.0f; d.angY = tilt; d.angZ = 1.0f;
}

/* Test cihazinin isini yap: 5 byte komut paketi kur, ayristiriciya yedir */
void injectCommand(uint8_t cmd) {
  uint8_t p[CMDIN_LEN] = { HDR_CMD, cmd, 0, FOOT1, FOOT2 };
  p[2] = checksum(&p[1], 1);
  for (int i = 0; i < CMDIN_LEN; i++) feedByte(p[i]);
}

/* Test cihazinin isini yap: 36 byte veri paketi kur, ayristiriciya yedir */
void injectData(const SensorData &d) {
  uint8_t p[DATA_LEN];
  p[0] = HDR_DATA;
  putFloat(&p[1],  d.altitude);  putFloat(&p[5],  d.pressure);
  putFloat(&p[9],  d.accX);      putFloat(&p[13], d.accY);   putFloat(&p[17], d.accZ);
  putFloat(&p[21], d.angX);      putFloat(&p[25], d.angY);   putFloat(&p[29], d.angZ);
  p[33] = checksum(&p[1], 32);
  p[34] = FOOT1;  p[35] = FOOT2;
  for (int i = 0; i < DATA_LEN; i++) feedByte(p[i]);
}

void runSelfTest() {
  Serial.println("=== SELF TEST: paket -> ayristirici -> algoritma ===");
  resetAll();

  /* 1) SUT Baslat'i gercek yoldan ver ve 1 sn gecikmenin islemesini bekle */
  injectCommand(CMD_SUT);
  Serial.println("SUT Baslat paketi verildi, 1 sn bekleniyor...");
  uint32_t w0 = millis();
  while (millis() - w0 < 1200) {
    if (sutPending && millis() - sutPendingAt >= 1000) {
      sutPending = false; sutActive = true; resetAll();
      Serial.println("SUT AKTIF");
    }
    delay(10);
  }
  if (!sutActive) { Serial.println("HATA: SUT aktiflesmedi! (ayristirici komutu yakalayamadi)"); return; }

  /* 2) 60 sn ucus: her 100 ms'de paket kur ve yedir */
  uint16_t prevBits = 0;
  uint32_t t0 = millis();
  while (true) {
    float t = (millis() - t0) / 1000.0f;
    if (t > 80.0f) break;

    SensorData olusan;
    syntheticProfile(t, olusan);
    injectData(olusan);                    // 36 byte kur + ayristir + isle

    uint16_t changed = statusBits & ~prevBits;
    for (int n = 0; n < 8; n++)
      if (changed & (1 << n))
        Serial.printf("t=%5.1f sn  BIT %d -> %s   (alt=%.1f prs=%.1f tilt=%.0f)\n",
                      t, n, BIT_NAMES[n], sd.altitude, sd.pressure, sd.angY);
    prevBits = statusBits;

    delay(100);                            // 10 Hz
  }

  /* 3) Durdur'u da gercek yoldan ver */
  injectCommand(CMD_STOP);

  Serial.println("=== SELF TEST BITTI ===");
  Serial.printf("Durdur sonrasi: sutActive=%d bits=0x%02X pyro1=%d pyro2=%d (hepsi 0 olmali)\n",
                sutActive, statusBits & 0xFF,
                digitalRead(PYRO1_PIN), digitalRead(PYRO2_PIN));
  Serial.println("Beklenen bit sirasi: 0 -> 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7");
  Serial.println("Tekrar icin RESET.");
}
#endif  /* SELF_TEST */

/* ================================================================
 *  SETUP / LOOP
 * ================================================================ */
void setup() {
  Serial.begin(115200);
  pinMode(PYRO1_PIN, OUTPUT);
  pinMode(PYRO2_PIN, OUTPUT);
  resetAll();

#if SELF_TEST
  delay(2000);           // seri monitoru acmaya vakit
  runSelfTest();
#else
  #if !USE_USB_AS_TESTPORT
    TestPort.begin(115200, SERIAL_8N1, TEST_RX_PIN, TEST_TX_PIN);
  #endif
#endif
}

void loop() {
#if !SELF_TEST
  pollTestPort();

  if (sutPending && millis() - sutPendingAt >= 1000) {
    sutPending = false; sutActive = true; resetAll();
  }

  static uint32_t lastTx = 0;
  if (sutActive && millis() - lastTx >= 100) {
    lastTx = millis();
    sendStatus(statusBits);
  }
#endif
}
