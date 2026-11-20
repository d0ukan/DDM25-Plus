# DDM32 Plus programlayıcı (Baofeng DM-32UV)

Windows programı: DDM32 Plus firmware'ini telsize kurar, günceller, eski sürüme geçer, telsizin yedeğini alır ve
yedekten geri yükler. Python gerekmez. **DDM32 Plus · d0ukan · TA2DOU** — <https://github.com/d0ukan/DDM32-Plus>

[English](README.en.md)

> [!CAUTION]
> **Sorumluluk size aittir.** DDM32 Plus resmî olmayan, deneysel bir yazılımdır; firmware yüklemek telsizi kalıcı
> olarak kullanılamaz hâle getirebilir. Program ilk açılışta sorumluluk reddinin tamamını gösterir; "Okudum, anladım
> ve kabul ediyorum" işaretlenmeden hiçbir telsiz düğmesi çalışmaz. Her yüklemeden önce yeniden sorulur:
> "Sorumluluk size aittir. Yedeğiniz hazır mı?"

## Kurulum

1. GitHub'daki son sürümden `DDM32Plus-<sürüm>.zip` dosyasını indirin ve bir klasöre açın (örneğin `Belgeler`).
2. `DDM32Plus\DDM32Plus.exe`'yi çalıştırın. Windows "bilinmeyen yayımcı" derse **Ek bilgi → Yine de çalıştır**.
3. Sorumluluk reddini okuyun, kutuyu işaretleyip **Kabul ediyorum**'a basın.

Yedekleriniz, indirilen sürümler ve ayarlar program klasöründe **değil**, `%LOCALAPPDATA%\DDM32Plus` içinde durur:

| Klasör | İçinde |
|---|---|
| `yedekler` | telsizin yedekleri, yükleme / geri yükleme kayıtları. **Silmeyin, kimseyle paylaşmayın**: kalibrasyon ve kanallarınız içindedir. |
| `surumler` | indirilen ve imzası denetlenen firmware sürümleri |
| `guncelleme` | program güncellemeleri |

Program güncellemesi bu klasöre dokunmaz.

## Kablo

Programlama kablosunu takın. Program kabloyu kendisi bulur: DM-32UV kablosu genelde **USB-SERIAL CH340** olarak
görünür; CP210x, FTDI, PL2303 gibi diğer USB-seri çipler de listelenir. Sanal portlar (com0com, Bluetooth) seçilmez.
Birden fazla kablo varsa bir seçim kutusu çıkar. Görünmüyorsa CH340 sürücüsünü kurun ve **Yenile**'ye basın.

## Telsizi güncelle

Her ekranda tek iş; telsize dokunan her adımın kendi düğmesi vardır:

1. **Hazırlık**: kablo ve sürüm. En yeni imzalı sürüm seçilir. İndirilmemişse **İndir ve doğrula**: imza ve SHA-256
   denetlenir, uymayan dosya kullanılmaz.
2. **Yedek**: telsiz kapalıyken **Yedek al**; sarı **"Telsizi şimdi açın"** yazısını görünce telsizi açın. Telsiz iki
   kez okunur, iki okuma aynı olmalıdır (sadece okuma).
3. **Kontrol** (port açılmaz, kendiliğinden): telsizde ne olduğu yedekten anlaşılır ve yazma planı hesaplanır.
   - **Stok yazılım** (.032 - .049): ilk kurulum. Telsizin kendi DMR codec'i yedeğinizden alınıp ayrılmış codec
     bölümüne hazırlanır; uygulama ve codec bölümü yazılır. DDM32 Plus yalnızca **stok .047** üzerinde denendi;
     diğer sürümlerde uyarı çıkar.
   - **Open25-DM32 Classic** ya da **DDM32 Plus**: güncelleme; yalnızca uygulama bölümü yazılır, codec bölümü korunur.
   - **Tanınmayan yazılım** (örneğin stok .050): kurulmaz. Önce Open25-DM32 Classic'i kendi Flasher'ıyla kurun, sonra
     DDM32 Plus'ı onun üzerine kurun.
   Bootloader ve veri çipi (kalibrasyon, kanallar) hiçbir zaman yazılmaz.
4. **Yükle**: onay penceresi sürümü, yazılacak bölümü, uyarıları ve "Sorumluluk size aittir. Yedeğiniz hazır mı?"
   uyarısını gösterir. Sonra telsizi kapatıp açın. Yükleme yedekle aynı telsiz olduğunu, telsizin yedekten beri
   değişmediğini denetler, yazar, her şeyi geri okuyarak doğrular. Hata olursa **kendiliğinden tekrar denenmez**.
5. **Bitti**: telsizi kontrol edin. Stoktan ilk kurulumda telsiz kanallar için "Yeni başlat" (Start new) sorabilir.

## Sürümler

- **Güncellemeleri denetle**: GitHub'daki imzalı DDM32 Plus sürümlerini listeler (beta ve kararlı işaretli).
  **Açılışta denetle** kapatılabilir; denetim yalnızca listeyi yeniler, hiçbir şey kurmaz.
- Yeni sürüm varsa ana sayfada ve bu sayfada **"Yeni sürüm var"** notlarıyla görünür.
- **Bu sürüme geç**: listedeki herhangi bir sürüm (eski sürümler dahil) her zaman yedek → kontrol → onay → yükleme
  yoluyla kurulur. Eski bir sürüme geçerken ayar / kanal biçimi geriye gidiyorsa uyarı çıkar ve ikinci bir onay
  istenir (örneğin yeni sürümde kaydedilmiş bir P25 kanalı eski sürümde farklı görünebilir).
- **Programı güncelle**: yeni program indirilir, imzası ve SHA-256'sı denetlenir; program kapanır, yenisiyle
  değiştirilir ve açılır. Yeni program açılmazsa eskisi geri gelir (eski klasör `DDM32Plus.old` olarak kalır).

İmzası programdaki anahtarla doğrulanmayan bir sürüm listede görünmez ve kurulamaz.

## Geri dön

- **Önceki sürüme dön**: son yüklemeden hemen önce alınan yedeği geri yükler.
- **Stok yazılıma dön**: telsiz stok yazılımdayken alınan **kendi** yedeğinizi geri yükler. Stok yazılım bu programla
  gelmez ve indirilmez; geri dönüş yalnızca bu telsizin yedeğiyle yapılır.

İkisi de uygulama ve codec bölümünü yedekten yazar ve geri okuyarak doğrular. Veri çipi (kanallar, ayarlar) yazılmaz:
DDM32 Plus'ta kaydedilen kanallar olduğu gibi kalır, stok yazılım bunları farklı gösterebilir; gerekirse stok CPS ile
yeniden yazın. **Yalnızca bu telsizin yedeğini kullanın.**

## Diğer sayfalar

- **Telsizin durumu**: bellek, hata kaydı ve açılış nedeni (sadece okuma).
- **Canlı panel**: sinyal, durum, P25 tarama sayaçları (sadece okuma; **Bağlan** portu açar, **Durdur** kapatır).
- **Kanallarım**: son yedekteki kanallar (sadece okuma; telsize yazmak için CPS kullanın).

## Sık sorulanlar

Program içindeki **Yardım** düğmesi: telsiz yakalanamadı, kablo bulunamadı, yedek eski, sürüm kurulamıyor, stoktan
ilk kurulum, kurulum yarıda kaldı, yedekler nerede.

## Teşekkürler

RadioFoundry (MIT), Open25-DM32 Classic (MIT, tylerwatt12) ve depodaki `THIRD_PARTY.md`'de sayılan projeler. DDM32
Plus bunlardan bağımsızdır; sorunları [DDM32 Plus'a](https://github.com/d0ukan/DDM32-Plus/issues) bildirin.
