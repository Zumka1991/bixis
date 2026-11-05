# Changelog

Формат основан на [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/),
версии следуют [SemVer](https://semver.org/lang/ru/).

## [Unreleased]

## [0.1.0] — 2026-11-05

Первый MVP терминала `bixis-term`.

### Добавлено

- Wayland-клиент: `xdg-shell`, серверные рамки через `xdg-decoration` (если композитор умеет),
  форма указателя через `cursor-shape-v1`, HiDPI через `preferred_buffer_scale`.
- Отрисовка в `wl_shm` с двумя буферами; перерисовываются только строки,
  отличающиеся от последнего кадра в этом буфере. Кадры синхронизированы с vblank.
- Шрифты через fontconfig + FreeType: regular/bold/italic/bold-italic,
  синтетический bold/italic, fallback-шрифты для недостающих символов, кэш глифов.
- Эмулятор VT: собственный парсер (ESC, CSI, OSC; DCS/APC/PM игнорируются), UTF-8,
  широкие символы (`wcwidth`), основной и альтернативный экраны (47/1047/1049),
  область прокрутки, origin mode, insert mode, табуляторы, DEC line drawing,
  SGR (bold, dim, italic, underline, reverse, invisible, strike, 16/256/truecolor),
  ответы на DA1/DA2/DSR, заголовок окна через OSC 0/2,
  режимы курсора и клавиатуры приложений, bracketed paste (2004), focus events (1004),
  форма курсора (`DECSCUSR`).
- Скроллбэк на 5000 строк в кольцевом буфере (строки хранятся обрезанными).
- Вкладки: `Ctrl+Shift+T` — новая, `Ctrl+Shift+W` — закрыть,
  `Ctrl+Tab` / `Ctrl+Shift+Tab` / `Ctrl+PgUp` / `Ctrl+PgDn` — переключить,
  `Ctrl+Shift+1…9` — перейти к вкладке по номеру. Полоска вкладок видна только при двух и более вкладках.
- Клавиатура через xkbcommon: автоповтор с параметрами композитора,
  Ctrl-сочетания и бинды работают в любой раскладке (берётся латинская).
- Выделение мышью, `Ctrl+Shift+C` — копировать, `Ctrl+Shift+V` и `Shift+Insert` — вставить.
- Прокрутка: колесо мыши, `Shift+PgUp` / `Shift+PgDn`, `Shift+Home` / `Shift+End`;
  в альтернативном экране колесо посылает стрелки (less, man).
- Масштаб шрифта: `Ctrl+=` / `Ctrl+-` / `Ctrl+0`.
- Настройка через `term/config.h`: шрифт, размер, отступы, палитра, скроллбэк, бинды.
- Каждая строка экрана хранит верхнюю границу занятых ячеек, поэтому прокрутка и запись
  в скроллбэк не зависят от ширины окна. Парсер обрабатывает 100–125 МБ/с.
- `make check`: тесты эмулятора под AddressSanitizer и UBSan.
- `make install` ставит бинарник и `bixis-term.desktop`; поддерживаются `PREFIX` и `DESTDIR`.
- Документация: `README.md`, `docs/MANUAL.md`, `ROADMAP.md`.
