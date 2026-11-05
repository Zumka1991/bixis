# Bixis

Минималистичная Wayland-оболочка на C. Собственный терминал со вкладками
и три приложения по горячим клавишам: браузер, мессенджер, видеоплеер.
Без панелей, анимаций и эффектов: только то, что нужно, и как можно быстрее.

Сейчас готов терминал **`bixis-term`** (v0.1), композитор `bixis` в планах.

## bixis-term

- Собственный парсер VT, 16/256/truecolor, альтернативный экран, скроллбэк
- Вкладки, выделение мышью, буфер обмена, bracketed paste
- Горячие клавиши работают в любой раскладке
- Перерисовываются только изменённые строки, кадры синхронизированы с vblank
- Около 100 КБ бинарник, около 14 МБ RSS, парсер выдаёт больше 100 МБ/с
- Зависимости: `wayland-client`, `xkbcommon`, `freetype2`, `fontconfig`

## Быстрый старт

Ubuntu / Debian:

```bash
sudo apt install build-essential pkgconf libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libfreetype-dev libfontconfig-dev
```

```bash
make && sudo make install
```

```bash
bixis-term
```

Главные клавиши: `Ctrl+Shift+T` — новая вкладка, `Ctrl+Tab` — следующая,
`Ctrl+Shift+C` / `Ctrl+Shift+V` — копировать / вставить, `Shift+PgUp` — история.

## Документация

- [Руководство](docs/MANUAL.md): установка на разных дистрибутивах, клавиши, настройка, решение проблем
- [Roadmap](ROADMAP.md)
- [Changelog](CHANGELOG.md)
