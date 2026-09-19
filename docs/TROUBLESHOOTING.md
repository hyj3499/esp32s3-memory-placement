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

---

## 4. heap tracing 이 mbedTLS 할당을 한 건도 기록하지 않음

**발생 시점:** Phase 2 (2026-09-19)
**환경:** ESP-IDF v5.5.5, `CONFIG_HEAP_TRACING_STANDALONE=y`,
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`

### 증상

첫 TLS 핸드셰이크를 heap tracing 으로 추적했는데 PSRAM 할당이 **0 건**이었다.

```
probe: alloc=271 free=697 기록=271/1024 최대=271 overflow=아니오
probe: 합계 내부=52274 PSRAM=0 / 살아있음 내부=572 PSRAM=0
```

`EXTERNAL` 빌드라 mbedTLS 할당이 전부 PSRAM 으로 가야 하고, `min_free` 로는 실제로
38,896 바이트가 움직이는 것을 이미 확인한 상태였다. 내부 히스토그램 합
(182+12+26+22+5+24 = 271)이 `기록=271` 과 정확히 같으므로 **필터로 걸러진 것이 아니라
애초에 기록이 없었다.**

더 이상했던 것은 내부 쪽 최대 할당이 2,048 바이트 이하뿐이라는 점이다.
`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384` 인데 **16 KB 버퍼가 내부에도 PSRAM 에도
없었다.**

### 원인

heap tracing 은 링커 `--wrap` 으로 네 함수를 가로챈다
(`components/heap/CMakeLists.txt:60`).

```
heap_caps_malloc_base
heap_caps_realloc_base
heap_caps_aligned_alloc_base
heap_caps_free
```

**`--wrap` 은 링크 시점에 해결되는 미정의 참조만 바꾼다.** 같은 오브젝트 파일 안에서
일어나는 호출은 컴파일 시점에 이미 해결되므로 가로채기를 타지 않는다.

`heap_caps_calloc_base()` 는 `heap_caps_base.c:332` 에서 `heap_caps_malloc_base()` 를
부른다 — **같은 파일이다.** ELF 디스어셈블이 그대로 보여준다.

```asm
heap_caps_calloc_base:
  call8  403767f0 <heap_caps_malloc_base>          ← 맨살
heap_caps_malloc:                  (heap_caps.c — 다른 파일)
  call8  403765c4 <__wrap_heap_caps_malloc_base>   ← 가로채짐
```

그리고 `esp_mbedtls_mem_calloc()`(`mbedtls/port/esp_mem.c:14`)은 `heap_caps_calloc()`
만 쓴다. **따라서 mbedTLS 의 모든 할당은 배치와 무관하게(INTERNAL 이든 EXTERNAL 이든)
heap tracing 에 잡히지 않는다.** `INTERNAL` 빌드로 되돌려 확인했을 때도 16 KB 버퍼는
나타나지 않았다.

맹점의 정확한 범위는 **`heap_caps_calloc()` 과 `heap_caps_calloc_prefer()`** 다.
`heap_caps_malloc()`, `heap_caps_aligned_alloc()`, newlib `malloc`/`calloc` 은 전부
다른 파일에서 `..._base` 를 부르므로 정상 추적된다 — 그래서 Wi-Fi, lwIP, AES 기록은
모두 잡혔다.

### 해결

측정 목적별로 나눠 썼다.

- **mbedTLS 의 총량**은 `min_free` 차분으로 잡는다(38,896). `EXTERNAL` 이 전부
  통째로 옮기므로 per-allocation 분해가 필요 없다
- **나머지 전부**는 heap tracing 으로 본다. 표적이던 "내부에 남은 47,039" 는 전부
  추적 가능한 경로였다
- 분류 결과를 쓸 때 "내부 소비의 82%" 가 아니라 **"추적된 내부 할당의 82%"** 로
  적는다

근본 해결이 필요하다면 `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC` +
`mbedtls_platform_set_calloc_free()` 로 후킹해 내부에서 `heap_caps_calloc` 대신
**`heap_caps_malloc`(가로채짐) + `memset`** 을 부르면 된다. 열 줄 남짓이고,
mbedTLS 할당 전체가 추적 대상이 된다.

### 교훈

**계측 도구의 침묵을 데이터로 읽으면 안 된다.**

`합계 PSRAM=0` 을 "PSRAM 을 안 쓴다" 로 읽었다면 결론 전체가 틀어졌을 것이다. 실제로
`min_free` 는 38,896 이 움직였다고 말하고 있었다. **두 계측이 어긋날 때는 둘 중
하나가 틀린 것이고, 어느 쪽인지는 도구를 열어봐야 안다.**

그리고 `--wrap` 의 이 성질은 IDF 만의 문제가 아니다. 링커 수준 가로채기를 쓰는 모든
계측(`--wrap`, `LD_PRELOAD`)이 **같은 번역 단위 내부 호출과 인라인된 호출을 놓친다.**
도구가 "무엇을 못 보는지" 를 먼저 확인하는 습관이 필요하다.

---

## 5. LVGL 차트의 점 마커가 크기를 0 으로 줘도 안 꺼짐

**발생 시점:** Phase 4, 첫 점등 (2026-09-19)
**환경:** ESP-IDF v5.5.5, `lvgl/lvgl` 9.6.0, `lv_chart`, 기본 테마

### 증상

대시보드가 처음 떴는데 오른쪽 차트 영역에 **흰 점이 흩뿌려져** 있었다. 선 그래프는
정상으로 그려지는데 데이터 점마다 작은 점이 하나씩 박혀 있다.

점 마커는 껐다고 생각한 상태였다.

```c
lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);  /* 점 표시 끔 */
```

### 원인

두 가지가 겹쳤다.

**① 크기 0 은 "안 그림" 이 아니라 "반지름 0" 이다.**
`lv_chart.c:1238` 이 스타일 크기를 그대로 쓰지 않고 반으로 나눠 반지름으로 쓴다.

```c
int32_t point_w = lv_obj_get_style_width_internal(obj, LV_PART_INDICATOR) / 2;
int32_t point_h = lv_obj_get_style_height_internal(obj, LV_PART_INDICATOR) / 2;
```

0 / 2 = 0 이므로 반지름 0 인 사각형, 즉 **1픽셀**이 점마다 그대로 찍힌다.
그리기를 건너뛰는 분기는 없다.

**② 색과 불투명도는 테마가 이미 박아 놨고, 나는 크기만 덮었다.**
기본 테마가 `LV_PART_INDICATOR` 에 스타일을 붙인다(`lv_theme_default.c:973`).

```c
lv_style_set_radius(&theme->styles.chart_indic, LV_RADIUS_CIRCLE);
lv_style_set_size(&theme->styles.chart_indic, chart_size, chart_size);   /* 8 */
lv_style_set_bg_color(&theme->styles.chart_indic, theme->base.color_primary);
lv_style_set_bg_opa(&theme->styles.chart_indic, LV_OPA_COVER);           /* :457 */
```

`lv_obj_set_style_size()` 는 이 중 **size 만** 덮는다. `bg_opa = LV_OPA_COVER` 가
그대로 남아 있으므로 마커는 계속 불투명하게 그려진다. 내가 한 일은 마커를 끈 것이
아니라 **8px 원을 1px 점으로 줄인 것**뿐이었다.

### 해결

크기가 아니라 불투명도로 끈다.

```c
lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);
```

`lv_draw_rect` 는 opa 가 투명이면 그리기 자체를 건너뛴다.

⚠️ 이 수정은 빌드만 통과한 상태다. 재플래시로 확인해야 한다.

### 교훈

**"크기 0" 이 "안 보임" 을 뜻한다고 가정하면 안 된다.** 위젯이 그 값을 어떻게
해석하는지는 구현에 달렸고, 여기서는 반지름이었다. 1px 이 남는 것은 0 으로 줄여도
사라지지 않는 종류의 잔재다.

그리고 **테마는 내가 건드리지 않은 속성을 이미 채워 놓았다.** 스타일 하나를
덮어썼다고 그 파트 전체를 통제하게 되는 것이 아니다. 무언가를 "끄려면" 끄려는
동작을 직접 지배하는 속성을 찾아야 한다 — 크기가 아니라 불투명도, 색이 아니라
`opa` 다. 이것도 `TROUBLESHOOTING #4` 와 성질이 같다: **도구/라이브러리가 무엇을
하는지 소스로 확인하기 전까지는 짐작이다.**
