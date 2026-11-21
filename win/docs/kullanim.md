# DDM32 Plus programı: kullanım kılavuzu

Baofeng DM-32UV telsize DDM32 Plus yazılımını kurmak, güncellemek, eski hâline döndürmek ve veri paketlerini yüklemek
için Windows programı (sürüm 2.0.0-beta.2). Python veya başka bir şey kurmanız gerekmez.

## 1. Kurulum

1. GitHub sürüm sayfasından `DDM32Plus-Setup-2.0.0-beta.2.exe` dosyasını indirin.
2. Çalıştırın. Program yalnızca sizin Windows kullanıcınız için kurulur, yönetici izni istemez
   (`%LOCALAPPDATA%\Programs\DDM32 Plus`). Başlat menüsüne ve isterseniz masaüstüne kısayol koyar.
3. **"Windows bilgisayarınızı korudu" uyarısı (SmartScreen):** program henüz kod imzalı değil. **Ek bilgi**'ye,
   sonra **Yine de çalıştır**'a tıklayın. Bu yalnızca ilk çalıştırmada çıkar.
4. İlk açılışta dili (Türkçe / English) seçin, sorumluluk reddini okuyup kabul edin.

Program yeni sürümleri kendisi denetler (Sürümler sayfasından kapatılabilir) ve imzası doğrulanmış güncellemeyi
kendisi kurar. Kaldırmak için: Ayarlar > Uygulamalar. **Yedekleriniz (`backups` klasörü) kaldırınca da silinmez.**

## 2. Gerekenler

- CH340 programlama kablosu ve sürücüsü. "Telsizin durumu" sayfası kabloyu ve sürücüyü denetler; sürücü yoksa ne
  yapacağınızı söyler.
- Telsizin pili dolu olsun; yazma sırasında kabloyu çekmeyin, telsizi kapatmayın.

## 3. Telsizi güncelle (sihirbaz)

**Telsizi güncelle** sekmesi beş adımdır:

1. **Hazırlık:** kabloyu takın, telsizi **kapatın**. Program kabloyu bulur. Listeden bir sürüm seçin
   (**Son sürüm** önerilir; **Beta** ve **Test / Alpha** sürümler uyarıyla gelir) ve "İndir ve doğrula"ya basın.
   Program dosyanın imzasını ve özetini (SHA-256) denetler.
2. **Yedek:** "Yedek al"a basın; sarı yazı "Telsizi şimdi kapatıp açın" deyince telsizi açın. Program iki yongayı
   ikişer kez okur ve karşılaştırır ("DOĞRULANDI"). Yedek, programın klasöründeki `backups` altına gider.
   - Telsizde zaten DDM32 Plus varsa, **"Bu telsizin son yedeğini kullan"** (hızlı güncelleme) seçilebilir: program
     önce telsizin kimliğini, önyükleme alanını ve codec yuvasını yedekle karşılaştırır.
   - **"Yedeksiz yükle"** önerilmez: kırmızı uyarı, "Anladım" kutusu ve ikinci bir onay ister.
3. **Kontrol:** program yedeği tanır (stok yazılım mı, DDM32 Plus mı) ve hangi bölümlerin yazılacağını gösterir.
4. **Yükle:** onaydan sonra yalnızca uygulama bölümleri yazılır; önyükleyici, codec yuvası ve veri yongası
   (kalibrasyon, kanallar) yazılmaz. Program her şeyi geri okuyup doğrular, sonra telsizi yeniden başlatır. Yazma
   başarısız olursa tekrar denenmez; "Geri dön" sayfası kullanılır.
5. **Bitti:** telsiz yeni yazılımla açılır.

## 4. Geri dön

**Geri dön** sekmesi telsizi kendi yedeğinizden geri yükler: bir önceki DDM32 Plus / Classic yedeği ya da stok
yazılım yedeği. Yedek seçilir, plan gösterilir, tek onayla yüklenir.

## 5. Veri paketleri

**Veri paketleri** sekmesi isteğe bağlıdır; telsiz paketsiz de çalışır. Telsizde DDM32 Plus v1.0.0-beta.4 veya
yenisi gerekir. Paketler veri yongasının yalnızca ayrılmış bölgesine (0xE70000-0xEFFFFF) yazılır.

- **RadioID paketi:** DMR kimliklerine ad gösterir. "radioid.net'i aç" ile sayfayı açıp `user.csv`'yi kendiniz
  indirin ya da "user.csv'yi indir"e basın (program dosyayı yalnızca o tıklamada radioid.net'ten indirir). Ülkeleri
  yazın (örneğin `Turkey`), "Paketi oluştur"a basın.
- **Hava bandı paketi:** meydanların frekansları. DHMİ eAIP'ten meydanların **AD 2** PDF sayfalarını kendiniz
  indirin, "AD 2 PDF ekle..." ile seçin, "Paketi oluştur"a basın. Program her sayfanın AD 2.18 tablosunu okur.
- **Telsize yaz:** telsiz **açık** olsun (kapatıp açmak gerekmez) ve DDM32 Plus çalışsın. "RadioID paketini yaz" ya
  da "Hava bandı paketini yaz"a basın, onaylayın. Yeni paket kullanımda olmayan yuvaya yazılır, başlığı en son;
  yazma bitene kadar telsiz eski paketini kullanır, bir sorun olursa eski paket kalır. Bitince telsizde
  Ayarlar > Hakkında yeni paketi gösterir.
- **Telsizde ne var:** "Telsizdeki paketleri oku" (yalnızca okur) ya da "Yedekten göster..." (bir yedek klasörü).

İndirdiğiniz listeler ve oluşan paketler programın `packs` klasöründe durur. Bu veriler sizin kullanımınız
içindir; başkasına dağıtmayın.

## 6. Telsizin durumu

**Telsizin durumu** sekmesi önce kabloyu ve CH340 sürücüsünü denetler (port açmaz). Telsiz **açık**, DDM32 Plus
çalışırken ve kablo takılıyken **Durumu oku**'ya basın: telsizin belleği, görevlerin yığınları, hata (çökme) kaydı ve
son açılış nedeni okunur. Yeşil ışık "Her şey yolunda", sarı "Dikkat: bellek az", kırmızı "Hata kaydı var" demektir;
kırmızıda "Ayrıntılar"daki çıktıyı bize gönderin. **her 30 sn oku** kutusu açıkken sayfa kendisi okur. Sayfa ayrıca
bu programın en yeni doğrulanmış yedeğini ve içindeki yazılımı gösterir (yalnızca dosyadan). Bu sayfa telsize hiçbir
şey yazmaz; yalnızca okuma komutları gönderilebilir, diğer her komut gönderilmeden reddedilir.

## 7. Canlı panel

**Canlı panel** telsizi birkaç saniyede bir okur: sinyal gücü (S-metre ve son okumaların çizgisi), durum, mod, kanal
ve bölge, frekans, P25 alıcısı, bellek, son açılış, hata kaydı ve P25 bölge tarama sayaçları. Telsiz açık ve kablo
takılıyken **Bağlan**'a basın; **Yenileme** ile 1-5 saniye seçilir (önce Durdur). **Durdur** okumayı bitirir ve portu
kapatır; program kapanırken de durur. Telsiz üç okumada hiç yanıt vermezse panel kendisi durur. Panel yalnızca okur:
ayar değiştirmez, yayın yapmaz. Panel açıkken başka bir sayfanın telsiz düğmesi beklemede kalır (kablo aynı anda tek
işe kullanılır).

## 8. Kanallarım

**Kanallarım** telsizin kanallarını yedekten gösterir; telsize bağlanmaz, hiçbir şey yazmaz. En yeni doğrulanmış
yedek kendiliğinden açılır; **Başka bir yedek aç...** ile başka bir yedek klasörü seçilir.

- **Kanallar** sekmesi: numara, ad, frekans, mod (FM, DMR, P25, NXDN, dPMR, YSF) ve kısa ayar (ton, CC/TS, NAC/TG,
  RAN ...). P25 NAC telsizdeki gibi önce ondalık yazılır: `659 ($293)`; herhangi NAC `Herhangi 3966 ($F7E)`.
  **Tüm sütunlar** kaydın bütün alanlarını gösterir. Sütun başlığına tıklayınca sıralanır.
- **Bölgeler** (en çok 68 bölge, bölge başına 80 kanal), **Kişiler**, **RX grup listeleri** sekmeleri. Bir bölgeye
  çift tıklayınca Kanallar sekmesi yalnızca o bölgenin kanallarını, bölgedeki sırayla gösterir (ya da **Bölge**
  listesinden seçin).
- **Ara:** yazdığınız metni her sütunda arar (ad, frekans, `659` ya da `$293` ...). **Mod** listesi kanalları moda
  göre süzer.
- **Gösterileni CSV olarak kaydet...** o sekmede gösterilen satırları bütün sütunlarıyla bir CSV dosyasına yazar
  (Excel açar).

Kanalları değiştirmek için telsizin kendi menülerini ya da CPS'i kullanın.

## 9. Programın klasörü

| Klasör | Ne var |
|---|---|
| `backups` | telsizin yedekleri ve kayıtlar (durum okumaları `health-*.log`): kalibrasyon ve kanallarınız içindedir, **silmeyin, paylaşmayın** |
| `releases` | indirilen yazılım sürümleri |
| `packs` | veri paketi dosyaları |
| `updates` | programın kendi güncellemesi |

2.0.0-beta.2 ve öncesinde bu klasörlerin adı Türkçeydi (`yedekler`, `surumler`, `paketler`, `guncelleme`); yeni
sürüm açılınca onları kendisi yeni adlara taşır, hiçbir şey silinmez.

Python programının eski yedekleri (`%LOCALAPPDATA%\DDM32Plus`, `DDM25Plus`) bulunur ve gösterilir; taşınmaz,
silinmez.

## 10. Sorun olursa

- **Kablo bulunamadı:** kabloyu başka bir USB yuvasına takın, "Telsizin durumu" sayfasındaki yönergeyi izleyin.
- **"Telsizi şimdi kapatıp açın" çıktı ama bir şey olmadı:** telsizi kapatıp yeniden açın; süre dolarsa adımı
  yeniden başlatın. Bu yalnızca okumadır, telsize bir şey yazılmamıştır.
- **Yazma yarıda kaldı:** program tekrar denemez. **Geri dön** sekmesinden son yedeğinizi geri yükleyin.
- **Durum okunamadı / Canlı panel "Bağlantı kesildi":** telsiz açık mı, DDM32 Plus çalışıyor mu, kablo takılı mı?
  Başka bir sayfada telsiz işlemi sürüyorsa bitmesini bekleyin.
- **Kanallarım "Yedek bulunamadı":** önce "Telsizi güncelle" sayfasında bir yedek alın ya da bir yedek klasörü seçin.
- Her işlemin ayrıntılı kaydı sayfanın "Ayrıntılar" bölümünde ve `backups` / `packs` klasöründeki `.log`
  dosyalarındadır.
