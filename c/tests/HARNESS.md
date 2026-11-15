# HARNESS.md — паритет-харнесс C vs Python (Semantic PRINCE, lab2)

Инфраструктура уровней L0-L1 из `results/lab2/c_port_plan.md` (раздел 5).
Состояние на 2026-11-14: эталонная сторона зелёная (39/39 PASS-PY),
C-подкоманды struct/pairs/chains/targeted/evaluate — stub -> SKIP,
гейт G1 (`spg dump`) уже побитово зелёный. Итоговый exit код — 0.

## Структура

```
c/tests/
  parity.sh            общий раннер (bash, без внешних зависимостей)
  gen_fixtures.py      детерминированный генератор всех мини-фикстур
  regressions.md       карта 12 багов (7+5) -> кейсы -> статусы
  HARNESS.md           этот файл
  fixtures/
    rt/                фикстуры редтима v1 (перенесены из /tmp/rt: uni4/bi4/
                       tpl/dig/sym, uni/bi K=10, test_b4, personal, leet)
                       + голдены fix_*, gs_*, p_*
    rt2/               фикстуры редтима v2 (tplZ/tplZ2/tplR, uni10/bi10,
                       uni3/bi3, crlf_test, pers_dup, test_dup, стримы)
                       + голдены p12*, c12*, cfull (до фикса) / cfull_fixed
    mini/              синтетический словарь на 30 слов для L1-прогонов
  work/                выводы прогонов (per-case *.py.out/*.c.out/*.rc,
                       parity.log с заголовком версий и sha256 входов)
```

/tmp летуч -> все входы воспроизводятся генератором:
`python3 gen_fixtures.py` (запись), `--check` (сверка байт-в-байт; сейчас
33 файла, 0 расхождений).

## Как запускать

```sh
cd c/tests
./parity.sh                 # полный прогон: эталон + C, skip-логика
./parity.sh --level L0      # только фикстурные регрессии
./parity.sh --case struct   # подстрока по имени кейса
./parity.sh --list          # список кейсов
PY_ONLY=1 ./parity.sh       # только эталон против голденов (контроль дрейфа)
SPG=./spg ./parity.sh       # нестандартный путь к бинарнику
```

Exit код: 0, если FAIL=0 (SKIP не считается ошибкой). FAIL(ref) означает,
что сломался Python-эталон или голден, а не C.

## Что делает раннер для каждого кейса

1. Прогоняет Python-эталон (`src/*.py`, с SP_FORCE_FULLSORT=1 /
   SP_FILLER_FULLSORT=1 в окружении — паритет-режим плана 5.2, пока no-op)
   и сверяет с голденом/счётчиком строк/rc/сообщением stderr — контроль
   дрейфа бейзлайна (риск R6).
2. Проверяет доступность C-стороны: бинарник исполняемый, подкоманда
   есть в usage `spg` и НЕ помечена `(stub)`. Иначе SKIP.
3. Прогоняет `spg <subcmd> <те же аргументы>`. Сверка:
   - rc-паритет (для ожидаемых ошибок H3: оба rc=1 + текст сообщения);
   - (а) побитово `cmp` stdout -> PASS(bit);
   - (б) при расхождении — каноническое: множества строк (`sort -u | cmp`)
     + метрики хитов: оба потока через `src/evaluate.py` на тест-сете кейса,
     финальные строки обязаны совпасть (дрейф промежуточных чекпоинтов на
     tie-order допустим и отражается в логе) -> PASS(canon), иначе FAIL.

Заголовок `work/parity.log`: версии python/numpy/cc, пути, sha256 всех
входных TSV/TXT (план 5.4).

## Покрытие сейчас (39 кейсов)

- L0 struct x8 (Б1, Б6, H1), pairs x5 (Б2), chains x8 (Б2, Б3, H3, H4),
  targeted x4 (Б4, Б5, H9), evaluate x3 (H7), dump x2 — гейт G1.
- L1 mini x7: struct budget+global, pairs pwbi+freq, chains lam=0/0.3,
  targeted --emit, dump.
- `spg-selftest` — заглушка под встроенный selftest из плана 4.1.

В SKIP (помечены `(stub)` в usage): struct, pairs, chains, targeted,
evaluate, selftest — 36 кейсов. Живые: dump x3 — PASS(bit), включая
чексумму logu (libm vs numpy на этих входах совпали побитово; полный
log/exp-скан плана 5.3 — отдельный этап).

## Контракт для C-разработчиков (чтобы кейсы снимались со SKIP сами)

1. `spg` без аргументов печатает usage со списком подкоманд; имя подкоманды
   в начале строки (после пробелов). Пока подкоманда не готова — держать
   пометку `(stub)` на её строке.
2. Имена и сигнатуры подкоманд — как в плане 4.1 (`spg struct uni bi tpl
   dig sym K LIMIT [alpha] [budget|global] [ntpl] [--no-dedup]` и т.д.);
   C-аргументы в кейсах совпадают с Python один-в-один.
3. Ошибки валидации (H3) — rc 1 и те же подстроки сообщений, что у Python
   («B (beam width)», «T (continuations», «lam must be»).
4. `emitted: N` — в stderr; stdout — только кандидаты, LF, хвостовой LF.
5. `spg dump` (формат tools/dump_ref.py) уже подключён — это образец.
