# Özgün Uçuş Kontrol Bilgisayarı — Sentetik Uçuş Testi (SUT) Uygulaması

TEKNOFEST Roket Yarışması finallerinde özgün geliştirilen uçuş kontrol bilgisayarları (Ö-UKB), sahadaki test cihazına bağlanarak iki senaryoda değerlendirilir: Sensör İzleme Testi (SİT) ve Sentetik Uçuş Testi (SUT). Bu depo, **SUT senaryosunun** Deneyap Kart üzerinde C++ ile gerçeklenmesini içerir.

Testin dayandığı şartname `docs/` klasöründedir: *Ek-7 — Özgün Uçuş Kontrol Bilgisayarlarının TEKNOFEST Roket Yarışması Finallerinde Test Edilmesi*. Aşağıdaki tablo ve alan tanımları bu dokümandan alınmıştır.

---

## SUT nedir?

Sentetik Uçuş Testi, uçuş kontrol bilgisayarının **kendi sensörlerini devre dışı bırakarak**, test cihazının seri port üzerinden gönderdiği yapay uçuş verilerini gerçek sensör ölçümü kabul etmesi ve uçuş algoritmasını bu veriler üzerinde çalıştırması esasına dayanır.

Amaç, roketi gerçekten uçurmadan şu soruların yanıtlanmasıdır:

- Gelen veriler doğru çözülüyor ve algoritmaya doğru aktarılıyor mu?
- Kurtarma sistemi (sürüklenme ve ana paraşüt) doğru anda tetikleniyor mu?
- Uçuşun hangi aşamasında olunduğu test cihazına düzenli bildiriliyor mu?
- Anormal veriler karşısında algoritma yanlış karar veriyor mu?

---

## Haberleşme parametreleri

| Parametre | Değer |
| --- | --- |
| Arayüz | RS232 (Ö-UKB tarafında UART + MAX3232 seviye dönüştürücü) |
| Baud Rate | 115200 bps |
| Veri uzunluğu | 8 bit |
| Parite | Yok |
| Stop biti | 1 |
| Mod | Full-Duplex |

Masaüstü geliştirme aşamasında MAX3232 yerine USB-TTL dönüştürücü kullanılabilir. Bayt düzeyinde protokol aynıdır; yalnızca hattaki gerilim seviyesi farklıdır. Saha testinde test cihazı RS232 seviyesinde çalıştığından seviye dönüştürücü zorunludur.

---

## Protokol özeti

Sistemde üç paket tipi kullanılır. Paketler Big Endian gönderim sırasına göre, header'dan footer'a doğru iletilir.

| Yön | Paket | Uzunluk | Başlık |
| --- | --- | --- | --- |
| Test cihazı → Ö-UKB | Komut (Başlat / Durdur) | 5 bayt | `0xAA` |
| Test cihazı → Ö-UKB | Sentetik uçuş verisi | 36 bayt | `0xAB` |
| Ö-UKB → Test cihazı | Durum bilgilendirme | 6 bayt | `0xAA` |

Başlık baytının iki farklı değer alması, alıcı tarafın paketi daha ilk baytta ayırt edebilmesi içindir: `0xAA` görüldüğünde 5 baytlık komut çerçevesi, `0xAB` görüldüğünde 36 baytlık veri çerçevesi beklenir.

### Komut seti

| İşlev | Header | Command | Checksum | Footer 1 | Footer 2 |
| --- | --- | --- | --- | --- | --- |
| SİT Başlat | `0xAA` | `0x20` | `0x8C` | `0x0D` | `0x0A` |
| SUT Başlat | `0xAA` | `0x22` | `0x8E` | `0x0D` | `0x0A` |
| Durdur | `0xAA` | `0x24` | `0x90` | `0x0D` | `0x0A` |

Başlat komutu alındığında, komutun doğruluğu onaylandıktan **bir saniye sonra** ilgili test modu etkinleştirilir. Durdur komutu alındığında tüm test bayrakları pasif duruma getirilir, veri gönderimi durdurulur ve Ö-UKB normal çalışma moduna dönerek yeni bir test oturumuna hazır hale gelir.

---

## Gelen sentetik veri paketi (36 bayt)

Paketin yapısı şu şekildedir:

```
+--------+---------------------------+----------+--------+--------+
| Header |     Veri alanı (32 bayt)  | Checksum | Foot 1 | Foot 2 |
|  0xAB  |   8 adet FLOAT32 (4'er)   |  1 bayt  |  0x0D  |  0x0A  |
+--------+---------------------------+----------+--------+--------+
   1 bayt          32 bayt              1 bayt    1 bayt   1 bayt
```

Veri alanındaki 32 bayt, dörder baytlık sekiz gruba ayrılır. Her grup bir FLOAT32 değeridir ve sırası sabittir:

| Bayt aralığı | Alan | Birim | Açıklama |
| --- | --- | --- | --- |
| 1–4 | İrtifa | m | Deniz seviyesine göre yükseklik |
| 5–8 | Basınç | mBar | Hava basıncı |
| 9–12 | İvme X | m/s² | Boylamsal eksen (burun yönü) |
| 13–16 | İvme Y | m/s² | Yanal eksen |
| 17–20 | İvme Z | m/s² | Dikey eksen |
| 21–24 | Açı X | ° | Yuvarlanma (roll) |
| 25–28 | Açı Y | ° | Yunuslama (pitch) |
| 29–32 | Açı Z | ° | Sapma (yaw) |

*(Bayt numaraları, başlık baytından sonraki veri alanı içinde 1'den başlatılmıştır; paket içindeki mutlak konumla aynıdır.)*

İvme ve açı verileri, roket gövdesine sabitlenmiş ve ağırlık merkezi referans alınmış Kartezyen eksen takımına göre tanımlanır. X ekseni burna doğru pozitif, Y ekseni sağa doğru pozitif, Z ekseni yeryüzüne doğru pozitiftir.

### Baytların çözülmesi

FLOAT32 değeri bellekte dört bayt yer kaplar. Test cihazı bu dört baytı sırayla hatta verir; alıcı taraf aynı sırayla birleştirerek değeri geri elde eder. Literatürde bu işlem *type casting* olarak geçer. Şartnamede `union` tabanlı bir örnek verilmiştir; bu depoda işlevsel olarak eşdeğer olan `memcpy` yaklaşımı kullanılmıştır:

```cpp
float getFloat(const uint8_t *src) {
  float v;
  memcpy(&v, src, 4);   // dört baytı float'ın bellek alanına kopyala
  return v;
}
```

Hem ESP32 hem de test cihazının çalıştığı x86 platformu little-endian olduğundan bayt sırası dönüştürülmez.

### Ayrıştırma ve algoritmaya aktarım

Çözülen sekiz değer, uçuş algoritmasının tek girdisi olan veri yapısına yazılır:

```cpp
struct SensorData{
float altitude,pressure;
float accX,accY,accZ;
float angX,angY,angZ;
};
SensorData sd;
```

Bu tasarımın temel gerekçesi şudur: **uçuş algoritması verinin kaynağını bilmez.** Normal uçuşta bu yapıyı kart üzerindeki sensörler doldurur; SUT sırasında ise seri porttan gelen paket doldurur. Şartnamenin "Ö-UKB kendi sensör verilerini görmezden gelerek test cihazından gelen verileri sensör verisi olarak kabul etmelidir" maddesi, uygulamada tam olarak bu tek satırlık kaynak değişimine karşılık gelir. Algoritmanın kendisinde hiçbir değişiklik gerekmez.

Paketin geçerliliği, algoritmaya aktarımdan önce üç aşamada denetlenir: footer baytlarının `0x0D 0x0A` olması, hesaplanan sağlama toplamının paket içindeki değerle eşleşmesi ve paketin beklenen uzunlukta tamamlanmış olması. Denetimlerden herhangi biri başarısız olursa paket işlenmeden atılır; bozuk bir çerçevenin uçuş kararlarını etkilemesi bu şekilde engellenir.

---

## Giden durum bilgilendirme paketi (6 bayt)

SUT süresince Ö-UKB, uçuş algoritmasının hangi aşamada olduğunu **10 Hz** (100 ms aralıkla) test cihazına bildirir. Gönderim, Durdur komutu alınana kadar kesintisiz sürer.

| Header | Data 1 | Data 2 | Checksum | Footer 1 | Footer 2 |
| --- | --- | --- | --- | --- | --- |
| `0xAA` | bit 0–7 | bit 8–15 | 1 bayt | `0x0D` | `0x0A` |

### Durum bitleri

Uçuş aşamaları, 16 bitlik tek bir durum kelimesinde (`uint16_t`) tutulur. Bir bitin değeri `1` ise ilgili aşama gerçekleşmiş, `0` ise henüz gerçekleşmemiştir. Bitler bir kez etkinleştikten sonra test sonuna kadar etkin kalır.

| Bit | Anlamı |
| --- | --- |
| 0 | Roket kalkışı algılandı |
| 1 | Motor yanma önlem süresi doldu |
| 2 | Minimum irtifa eşiği aşıldı |
| 3 | Gövde açısı / yanal ivme sınırı aşıldı |
| 4 | Roket irtifası alçalmaya başladı |
| 5 | Sürüklenme paraşütü açma emri oluşturuldu |
| 6 | Roket irtifası belirlenen değerin altına indi |
| 7 | Ana paraşüt açma emri oluşturuldu |
| 8–15 | Rezerve |

### 16 bitin iki bayta bölünmesi

Seri hat üzerinden bir seferde tek bayt iletilebildiğinden, 16 bitlik durum kelimesi iki parçaya ayrılır:

```cpp
p[1] = (uint8_t)(bits & 0xFF);   // Data 1 -> ilk 8 bit  (bit 0-7)
p[2] = (uint8_t)(bits >> 8);     // Data 2 -> son 8 bit  (bit 8-15)
```

`& 0xFF` maskesi üst sekiz biti temizleyerek alt yarıyı yalıtır. `>> 8` kaydırması ise üst sekiz biti alt konuma indirir. Alıcı taraf `Data1 | (Data2 << 8)` işlemiyle 16 bitlik kelimeyi yeniden oluşturur.

Bit 8–15 aralığı şartnamede rezerve edildiğinden Data 2 alanı bu uygulamada daima `0x00` değerini taşır. Alan, çerçeve uzunluğunun korunması için yine de gönderilir.

Örnek: bit 0, 1, 2 ve 4 etkin ise durum kelimesi `0b0000000000010111` olur; Data 1 = `0x17`, Data 2 = `0x00`.

### Bit işlemleri

Durum bitlerinin yönetiminde iki temel işlem kullanılır:

```cpp
statusBits |= (1 << 5);          // 5 numaralı biti etkinleştir
if (statusBits & (1 << 5)) { }   // 5 numaralı bit etkin mi?
```

`1 << n` ifadesi, yalnızca n numaralı biti `1` olan bir maske üretir. `|=` işleci bu maskeyi mevcut değerle birleştirir ve diğer bitleri korur; `&` işleci ise yalnızca sorgulanan biti süzer, sonuç sıfırdan farklıysa bit etkindir.

Bu bitler aynı zamanda **tekrar koruması** görevi görür. Kurtarma çıkışları tetiklenmeden önce ilgili bitin daha önce etkinleşmediği doğrulanır; böylece 10 Hz'lik döngüde aynı pyro çıkışının defalarca sürülmesi engellenir.

---

## Sağlama toplamı

Şartnamede sağlama toplamı, ilgili baytların toplanması ve sonucun 256'ya bölümünden kalanının alınması olarak tanımlanmıştır:

```cpp
uint8_t checksum(const uint8_t *b, size_t n) {
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++) s += b[i];
  return (uint8_t)(s & 0xFF);
}
```

> **Not:** Şartnamede verilen örnek değerler (`0x20` komutu için `0x8C`, `0x14 0x00` durumu için `0x1A`) bu tanımla birebir doğrulanamamaktadır. Bu nedenle mevcut uygulamada gelen komut paketlerinde sağlama toplamı uyuşmazlığı paketi geçersiz kılmaz; yalnızca kayda alınır. Resmî masaüstü test yazılımı ile yapılacak ilk bağlantıda gerçek bayt dizisi gözlemlenerek formül kesinleştirilmeli ve denetim sıkı moda alınmalıdır.

---

## Test oturumunun akışı

1. Test cihazı ile Ö-UKB arasında RS232 bağlantısı ve pyro çıkışlarının klemens bağlantıları yapılır.
2. Test cihazı `SUT Başlat` komutunu gönderir.
3. Ö-UKB komutu doğrular ve bir saniye sonra SUT modunu etkinleştirir.
4. Test cihazı 36 baytlık sentetik veri paketlerini akıtır; Ö-UKB her paketi çözerek uçuş algoritmasına verir.
5. Ö-UKB eş zamanlı olarak 10 Hz ile durum bilgilendirme paketini gönderir.
6. Önceden tanımlanmış koşullar sağlandığında kurtarma çıkışları tetiklenir ve ilgili durum bitleri etkinleşir.
7. Test cihazı `Durdur` komutunu gönderir; Ö-UKB tüm bayrakları temizler, çıkışları kapatır ve normal moda döner.

Test, farklı uçuş profilleriyle birden fazla kez tekrarlanabilir.

---

## Uçuş algoritması

Tepe noktası kararı tek bir ölçüme dayandırılmaz. Barometrik irtifa doğası gereği gürültülüdür ve tek örneklik bir düşüş, roket hâlâ yükselirken de gözlemlenebilir. Bu nedenle karar üç bağımsız göstergenin birlikte değerlendirilmesiyle verilir:

- **İrtifa göstergesi:** ölçülen irtifa, o ana kadarki en yüksek değerin altında ve ardışık olarak belirli sayıda örnek boyunca bu durumda kalıyorsa alçalma kabul edilir.
- **Basınç göstergesi:** alçalma sırasında hava basıncı artar; ölçülen basınç, en düşük değerin üzerinde ve ardışık örneklerde bu durumda kalıyorsa alçalma kabul edilir.
- **Yatma göstergesi:** gövde açısının dikeyden sapması tanımlı sınırı aşmışsa roketin yörünge tepesine yaklaştığı değerlendirilir.

Tepe noktası, irtifa ve basınç göstergelerinin birlikte alçalma bildirmesi durumunda ya da yatma göstergesinin etkin olması ve diğer iki göstergeden en az birinin alçalma bildirmesi durumunda onaylanır. Yatma göstergesi tek başına yeterli sayılmaz; roket yükseliş aşamasında da eğik seyredebilir.

Kurtarma çıkışları ayrıca iki ön koşula bağlıdır: motor yanma önlem süresinin dolmuş olması ve minimum irtifa eşiğinin aşılmış olması. Bu koşullar, kalkış anındaki titreşimlerin veya rampa üzerindeki hareketlerin erken ayırmaya yol açmasını engeller.

Eşik değerleri kaynak dosyanın başında sabit olarak tanımlanmıştır ve roketin uçuş profiline göre ayarlanmalıdır.

---

## Depo içeriği

| Dosya | Açıklama |
| --- | --- |
| `sut_deneyap.ino` | Ö-UKB SUT uygulaması (Deneyap Kart / ESP32) |
| `docs/Ek-7_Ozgun_UKB_Testleri.docx` | Şartname |

---

## Derleme ve çalıştırma

Kaynak dosya Arduino IDE ile derlenir. Deneyap Kart desteğinin kart yöneticisine eklenmiş olması gerekir.

Dosyanın başındaki `SELF_TEST` tanımı iki çalışma kipini belirler:

- `SELF_TEST 1` — Kart, sentetik uçuş profilini kendi içinde üretir, şartnameye uygun 36 baytlık paketlere dönüştürür ve kendi ayrıştırıcısına verir. Böylece paket oluşturma, ayrıştırma, sağlama toplamı ve algoritma zinciri harici donanım olmadan doğrulanır. Sonuçlar seri monitörden (115200 bps) izlenir.
- `SELF_TEST 0` — Saha kipi. Veriler harici test cihazından veya resmî masaüstü test yazılımından alınır.

Pyro çıkış pinleri ve eşik değerleri kullanılan donanıma göre kaynak dosyanın başından güncellenmelidir.

---

## Kaynak

TEKNOFEST Roket Yarışması — *Ek-7: Özgün Uçuş Kontrol Bilgisayarlarının TEKNOFEST Roket Yarışması Finallerinde Test Edilmesi*
