# 트러블슈팅 기록

개발하며 실제로 겪은 문제만 기록한다. 증상 → 원인 → 해결 → 교훈 순서로 적고,
로그는 각색하지 않고 그대로 붙인다. 나중에 몰아서 쓰면 기억나지 않는다.

---

## 1. PSRAM 활성화 후 부팅 무한 재부팅

**발생 시점:** Phase 0 (2026-09-18)
**환경:** ESP-IDF v5.5.5, Guition JC4827W543 (ESP32-S3), Windows

### 증상

`menuconfig`에서 PSRAM을 켠 직후부터 부팅에 실패하고, abort → 재부팅을 무한 반복했다.

```
E (153) quad_psram: PSRAM chip is not connected, or wrong PSRAM line mode
E cpu_start: Failed to init external RAM!

abort() was called at PC 0x42001637 on core 0
--- 0x42001637: mspi_init at components/esp_system/port/cpu_start.c:616
```

앱 코드는 `app_main()`이 비어 있는 상태였으므로 애플리케이션 문제가 아니었다.

### 원인

PSRAM **라인 모드를 Quad로 설정**했는데, 실제 칩은 **Octal**이었다.

보드 기판 각인은 `JC4827W543` / `Model XH-S3E`뿐이고, 흔한 `N16R8` 형식의
플래시/PSRAM 표기가 없었다. 플래시가 4MB로 감지된 것을 근거로 `N4R2`(4MB 플래시 +
2MB Quad PSRAM) 조합이라고 추정한 것이 틀렸다.

이 보드의 PSRAM은 **칩 내장(in-package)** 이라 모듈 외형 표기만으로는 알 수 없다.

### 해결

`menuconfig` → `Component config` → `ESP PSRAM` → `SPI RAM config` →
`Mode (QUAD/OCT) of SPI RAM chip in use` → **Octal Mode PSRAM**

결과:

```
I (154) octal_psram: vendor id    : 0x0d (AP)
I (154) octal_psram: density      : 0x03 (64 Mbit)
I (192) esp_psram: Found 8MB PSRAM device
I (196) esp_psram: Speed: 40MHz
I (933) esp_psram: SPI SRAM memory test OK
I (1009) esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
```

### 교훈

**PSRAM 사양은 보드 모델명으로 추정하지 말고 esptool 출력에서 확인한다.**

`idf.py flash`가 칩에 접속할 때 이미 정답을 출력하고 있었다.

```
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
```

`Features:` 줄에 PSRAM 용량과 전압 타입이 찍힌다. 8MB / AP_3v3는 Octal이다.
플래시를 굽지 않고 확인만 하려면:

```
esptool.py --chip esp32s3 -p COM9 chip_id
```

추측의 근거(플래시 4MB → N4R2)가 그럴듯했기 때문에 더 오래 헤맸다. 확인할 수 있는
정보가 이미 눈앞에 출력되고 있는데 추론으로 대신한 것이 실제 원인이다.

### 참고 — 확정된 하드웨어 사양

| 항목 | 값 |
|---|---|
| 플래시 | 4MB (DIO, 80MHz) |
| PSRAM | 8MB Octal, AP 3V, 40MHz |
| 내부 SRAM 가용 | 약 390KB (Wi-Fi 미구동 기준) |

관련 설정 (`sdkconfig`):

```
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_40M=y
```

---

## 2. IDF 설치 폴더에서 `idf.py` 실행

**발생 시점:** Phase 0 (2026-09-18)

### 증상

```
CMake Error at CMakeLists.txt:4 (message):
  Current directory 'C:/Espressif/frameworks/esp-idf-v5.5.5' is not
  buildable.
```

### 원인

ESP-IDF 명령 프롬프트를 새로 열면 항상 IDF 설치 폴더에서 시작한다. `idf.py`는
**현재 작업 디렉터리를 프로젝트 루트로 간주**하므로, 프로젝트로 이동하지 않고
실행하면 프레임워크 원본을 빌드하려 시도한다.

### 해결

프로젝트 폴더로 이동 후 실행. 실패한 실행이 IDF 트리에 `build/`를 만들어놓으므로
같이 지운다.

### 교훈

`idf.py`를 치기 전에 프롬프트에 찍힌 경로가 프로젝트 루트인지 확인한다. ESP-IDF는
프로젝트 위치를 환경변수가 아니라 **cwd**로 판단한다.

---

## 3. `heap_trace_dump_caps()` 호출 직후 인터럽트 워치독으로 죽음

**발생 시점:** Phase 2 (2026-09-19)
**환경:** ESP-IDF v5.5.5, `CONFIG_HEAP_TRACING_STANDALONE=y`, 콘솔 115200 baud

### 증상

TLS 핸드셰이크 한 번을 heap tracing으로 추적하고 `heap_trace_dump_caps()`로 덤프하자,
덤프가 **중간에 잘리면서** 코어가 죽었다. 리셋 후 재시도해도 매번 같은 자리쯤에서
죽는다. 4회 연속 재현.

```
I (9308) probe: alloc=264 free=691 기록=264/1024 최대=264 overflow=아니오
I (9308) probe: ---- 내부 SRAM 할당 덤프 ----
====== Heap Trace: 264 records (1024 capacity) ======
   224 bytes (@ 0x3fcb964c, Internal) allocated CPU 0 ccount 0x495656ac ...
   (약 25개 출력 후)
    18 bytes (@ 0x3fcb9a38, Internal) allocated CPU 0Guru Meditation Error:
    Core  1 panic'ed (Interrupt wdt timeout on CPU1).
```

패닉 메시지가 **출력 줄 한가운데를 끊고** 끼어든 점이 단서였다. 출력하는 도중에
죽었다는 뜻이다.

### 원인

`heap_trace_dump_base()`가 **덤프 전체를 하나의 critical section 안에서** 돌린다.

```c
static void heap_trace_dump_base(bool internal_ram, bool psram)
{
    portENTER_CRITICAL(&trace_mux);          // heap_trace_standalone.c:343
    ...
        esp_rom_printf("%6d bytes (@ %p%s) allocated CPU %d ccount 0x%08x", ...);
    ...                                       // 레코드마다 여러 번
    portEXIT_CRITICAL(&trace_mux);
}
```

`esp_rom_printf`는 UART가 비워질 때까지 도는 블로킹 출력이다. 레코드 264개에
레코드당 약 100바이트면 **26KB**이고, 115200 baud에서 약 **2.3초**가 걸린다.
그 시간 내내 인터럽트가 꺼져 있으므로 인터럽트 워치독(기본 300ms)이 먼저 터진다.

즉 **버그가 아니라 규모 문제**다. 레코드 수십 개짜리 예제에서는 드러나지 않는다.

### 해결

`heap_trace_dump_caps()`를 쓰지 않고 직접 순회했다. `heap_trace_get()`은 **호출마다**
락을 잡고 놓으므로(`heap_trace_standalone.c:269`) critical section이 레코드 하나
길이로 짧아진다.

```c
size_t n = heap_trace_get_count();
for (size_t i = 0; i < n; i++) {
    heap_trace_record_t r;
    if (heap_trace_get(i, &r) != ESP_OK) break;
    if (!esp_ptr_internal(r.address)) continue;   // 표적은 내부 SRAM
    ESP_LOGI(TAG, "R,%u,%u,%p,%d,%p,%p", ...);
}
```

`ESP_LOGI`는 UART VFS를 거치므로 출력 중에 태스크가 양보한다. 덤으로 출력 형식을
직접 정할 수 있어 CSV로 뽑아 후처리가 쉬워졌고, `esp_ptr_internal()`로 걸러
PSRAM 레코드를 빼면서 줄 수도 줄었다.

콘솔 baud를 921600으로 올리는 방법도 있지만 2.3초 → 0.29초일 뿐이라 300ms 한계에
여전히 아슬아슬하다. **원인이 시간이 아니라 critical section이므로 그쪽을 고쳐야 한다.**

### 교훈

**IDF가 주는 편의 함수라고 해서 내 규모에서 안전한 것은 아니다.**

`heap_trace_dump_caps()`는 API 문서에 "It is safe to call this function while heap
tracing is running"이라고만 적혀 있고, 얼마나 오래 인터럽트를 끄는지는 말하지 않는다.
증상이 "출력 중간에 죽음"이었으므로 **출력 코드 자체를 읽는 것**이 가장 빨랐다.

계측 코드가 관측 대상을 오염시키는 문제(Observer Effect)의 또 다른 얼굴이기도 하다.
이 프로젝트는 계측 태스크를 전부 정적으로 올려 **메모리** 오염을 막았는데, 여기서는
**시간** 쪽으로 같은 문제가 나왔다.
