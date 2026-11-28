# Регрессионная карта: 14 багов (bugfixes_v1 + bugfixes_v2) -> кейсы parity.sh

Запуск любой строки: `./parity.sh --case <кейс>` (из `c/tests/`).
Полный прогон: `./parity.sh` (C-сторона), `PY_ONLY=1 ./parity.sh` (только
эталон против голденов).

Статусы на 2026-11-27:
- **Python-эталон**: все кейсы зелёные против голденов/счётчиков/rc.
- **C (spg)**: все подкоманды реализованы, кейсы зелёные — 34 PASS(bit),
  2 PASS(canon) (chains-rt-lam05, chains-h4-full: tie-order внутри групп
  ничьих, множества и метрики совпадают), 4 PASS(rc) (struct-h8-ntpl0,
  chains-h3-b0/t0/lam2). SKIP только spg-selftest (не из этой карты).

## bugfixes_v1 (Б1-Б7)

| Баг | Суть фикса | Кейс(ы) parity.sh | Эталон (Python) | C |
|-----|-----------|-------------------|-----------------|---|
| Б1 | budget: квоту жгут только эмитированные; ceil-квоты; раунды с перераспределением | `struct-rt-budget` (+ эталон множества `struct-rt-global`) | OK: 16 строк = голден `rt/fix_budget.txt`; множество == global (голден `rt/gs_global.txt`) | PASS(bit) |
| Б2 | top-limit срез ПОСЛЕ дедупа (топ-ап глубины) | `pairs-rt-pwbi`, `pairs-rt-pwbi-nodedup`, `pairs-rt2-pwbi`, `pairs-rt2-pwbi-nodedup`, `chains-rt-12` | OK: ровно 12 строк, добрана `xxaba` (голдены `rt/fix_pairs*.txt`, `rt2/p12*.txt`, `rt2/c12.txt`) | PASS(bit) |
| Б3 | краш argpartition при T > K | `chains-b3-t80` | OK: T=80 при K=10 -> rc 0, 12 строк (голден `rt2/c12t80.txt`) | PASS(bit) |
| Б4 | регистрация хита по ВСЕМ сплитам | `targeted-b4` | OK: «personal words: 2, test slice: 2 (reachable: 2)», оба пароля хитят (2/2) | PASS(bit) |
| Б5 | --service в начало personal при любом источнике | `targeted-b5-service` | OK: stderr «personal words: 6» (4 сервисных + 2 личных) | PASS(bit) |
| Б6 | флаги фильтруются из позиционных слотов | `struct-b6-flag-mid`, `struct-b6-flag-pos` | OK: rc 0, 10 строк в обоих порядках аргументов | PASS(bit) |
| Б7 | heap vs grid: ~1 ulp на ничьих (документировано) | косвенно: `struct-mini-budget` / `struct-mini-global` (grid-путь) + побитовый cmp когда struct реализован | OK (эталон стабилен на голденах mini) | PASS(bit) |

## bugfixes_v2 (H1, H3, H4, H6, H7, H8, H9)

| Баг | Суть фикса | Кейс(ы) parity.sh | Эталон (Python) | C |
|-----|-----------|-------------------|-----------------|---|
| H1 | ZeroDivisionError/log(0) на нулевых счётчиках шаблонов | `struct-h1-zero-budget` (0 строк), `struct-h1-zero-global` (0), `struct-h1-zero2-budget` (12), `struct-h1-zero2-global` (10) | OK: все rc 0, счётчики строк совпали с bugfixes_v2 | PASS(bit) |
| H3 | валидация B / T / LAM | `chains-h3-b0`, `chains-h3-t0`, `chains-h3-lam2` | OK: rc 1 + сообщения «B (beam width)», «T (continuations», «lam must be» | PASS(rc) |
| H4 | обрыв по -inf внутри чанка 500K | `chains-h4-full` (+ `chains-rt-12`) | OK: 694 строки = голден `rt2/cfull_fixed.txt`, побитовый префикс дофиксных 958 (`rt2/cfull.txt`) | PASS(canon) |
| H6 | дубли строк тест-файла: не фикс, поведение задокументировано (латентная метрика) и запинено кейсом | `targeted-h6-dup` | OK: stderr «test slice: 2» | PASS(bit) |
| H7 | CRLF в тест-сете/потоке | `eval-crlf-set`, `eval-crlf-stream` | OK: 2/2 cracked (100%) на CRLF-сете; CRLF-поток vs LF-сет 1/1 | PASS(bit) |
| H8 | NTPL=0: раньше молча значил «все шаблоны», теперь явная ошибка | `struct-h8-ntpl0` | OK: rc 1 + «NTPL (templates cap)» | PASS(rc) |
| H9 | дедуп слов в --personal | `targeted-h9-dup` | OK: stderr «3 words -> 2», «personal words: 2»; slice/reachable без изменений | PASS(bit) |

## Гейт G1 (не из списка багов, но уже живой)

| Проверка | Кейс(ы) | Эталон | C |
|----------|---------|--------|---|
| Чексуммы массивов модели (sig, words_blob, counts, bi_i, bi_j, bi_cnt, logu) | `dump-rt`, `dump-rt2`, `dump-mini` | `c/tools/dump_ref.py` | **PASS(bit)** — побитово, включая logu (libm log совпал с numpy на этих входах) |

## Правила снятия статуса SKIP

1. Подкоманда считается готовой, когда строка в usage `spg` больше не содержит
   пометки `(stub)`; харнесс подхватит её автоматически.
2. Побитовое расхождение на ничьих — не FAIL автоматически: срабатывает
   каноническое сравнение (множества строк + метрики evaluate), см. HARNESS.md.
3. Красный эталон (FAIL(ref)) означает дрейф Python-стороны или голденов —
   чинить эталон/фикстуру, а не C.
