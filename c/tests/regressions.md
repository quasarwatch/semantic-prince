# Регрессионная карта: 12 багов (bugfixes_v1 + bugfixes_v2) -> кейсы parity.sh

Запуск любой строки: `./parity.sh --case <кейс>` (из `c/tests/`).
Полный прогон: `./parity.sh` (C-сторона), `PY_ONLY=1 ./parity.sh` (только
эталон против голденов).

Статусы на 2026-11-14:
- **Python-эталон**: проверен живым прогоном (PASS-PY), доказательство в колонке
  «Эталон».
- **C (spg)**: подкоманды struct/pairs/chains/targeted/evaluate — stub
  (usage помечен `(stub)`), кейсы в SKIP. Гейт G1 (`spg dump` — чексуммы
  массивов модели) уже реализован и побитово зелёный.

## bugfixes_v1 (Б1-Б7)

| Баг | Суть фикса | Кейс(ы) parity.sh | Эталон (Python) | C |
|-----|-----------|-------------------|-----------------|---|
| Б1 | budget: квоту жгут только эмитированные; ceil-квоты; раунды с перераспределением | `struct-rt-budget` (+ эталон множества `struct-rt-global`) | OK: 16 строк = голден `rt/fix_budget.txt`; множество == global (голден `rt/gs_global.txt`) | SKIP (stub) |
| Б2 | top-limit срез ПОСЛЕ дедупа (топ-ап глубины) | `pairs-rt-pwbi`, `pairs-rt-pwbi-nodedup`, `pairs-rt2-pwbi`, `pairs-rt2-pwbi-nodedup`, `chains-rt-12` | OK: ровно 12 строк, добрана `xxaba` (голдены `rt/fix_pairs*.txt`, `rt2/p12*.txt`, `rt2/c12.txt`) | SKIP (stub) |
| Б3 | краш argpartition при T > K | `chains-b3-t80` | OK: T=80 при K=10 -> rc 0, 12 строк (голден `rt2/c12t80.txt`) | SKIP (stub) |
| Б4 | регистрация хита по ВСЕМ сплитам | `targeted-b4` | OK: «personal words: 2, test slice: 2 (reachable: 2)», оба пароля хитят (2/2) | SKIP (stub) |
| Б5 | --service в начало personal при любом источнике | `targeted-b5-service` | OK: stderr «personal words: 6» (4 сервисных + 2 личных) | SKIP (stub) |
| Б6 | флаги фильтруются из позиционных слотов | `struct-b6-flag-mid`, `struct-b6-flag-pos` | OK: rc 0, 10 строк в обоих порядках аргументов | SKIP (stub) |
| Б7 | heap vs grid: ~1 ulp на ничьих (документировано) | косвенно: `struct-mini-budget` / `struct-mini-global` (grid-путь) + побитовый cmp когда struct реализован | OK (эталон стабилен на голденах mini) | SKIP (stub) |

## bugfixes_v2 (H1, H3, H4, H7, H9)

| Баг | Суть фикса | Кейс(ы) parity.sh | Эталон (Python) | C |
|-----|-----------|-------------------|-----------------|---|
| H1 | ZeroDivisionError/log(0) на нулевых счётчиках шаблонов | `struct-h1-zero-budget` (0 строк), `struct-h1-zero-global` (0), `struct-h1-zero2-budget` (12), `struct-h1-zero2-global` (10) | OK: все rc 0, счётчики строк совпали с bugfixes_v2 | SKIP (stub) |
| H3 | валидация B / T / LAM | `chains-h3-b0`, `chains-h3-t0`, `chains-h3-lam2` | OK: rc 1 + сообщения «B (beam width)», «T (continuations», «lam must be» | SKIP (stub) |
| H4 | обрыв по -inf внутри чанка 500K | `chains-h4-full` (+ `chains-rt-12`) | OK: 694 строки = голден `rt2/cfull_fixed.txt`, побитовый префикс дофиксных 958 (`rt2/cfull.txt`) | SKIP (stub) |
| H7 | CRLF в тест-сете/потоке | `eval-crlf-set`, `eval-crlf-stream` | OK: 2/2 cracked (100%) на CRLF-сете; CRLF-поток vs LF-сет 1/1 | SKIP (stub) |
| H9 | дедуп слов в --personal | `targeted-h9-dup` | OK: stderr «3 words -> 2», «personal words: 2»; slice/reachable без изменений | SKIP (stub) |

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
