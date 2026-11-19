# AotR Access

Мод, который делает Age of the Ring доступной для незрячих игроков: игра говорит через экранный диктор.

English below.

## Что это

- Age of the Ring — бесплатный мод для «Властелин Колец: Битва за Средиземье 2 — Под знаменем Короля-чародея». Ставится одним установщиком с сайта ageofthering.net.
- AotR Access подключается к игре и озвучивает её через ваш экранный диктор с помощью библиотеки Prism. Проверено с NVDA.
- Мод говорит по-русски.
- Это ранняя тестовая версия. Многое уже работает, многое ещё нет. Пожалуйста, сообщайте о проблемах во вкладке Issues.

## Что уже умеет

- Читает меню: главное меню, кампании, схватку, настройки, сохранения, диалоги.
- В бою есть сканер: список того, что вокруг, с расстоянием и направлением, по категориям: мои, союзники, чужие, нейтральные, цели, события.
- Цели миссий кампании: что делать и куда идти, по шагам. Пройдено много миссий кампании «Властелин Колец».
- Приказы без мыши: идти, атаковать, «с боем к», маршрут к цели.
- Режим команд: кнопки выделенного отряда или здания, книга заклинаний, магазин заклинаний.
- Сводки боя: где враги и сколько их.
- Карта «Войны Кольца»: ходы, армии, постройки (частично).
- Меню тренировки для новичков: деньги, очки командования, бессмертие своих отрядов.

## Что нужно

- Age of the Ring 10.0.2 (отдельная установка со своим лаунчером).
- Windows, экранный диктор (NVDA рекомендуется).
- Мод проверен на версии игры 2.01 (файл rotwk\game.dat). После обновления игры мод может отключиться, пока его не обновят.

## Установка

1. Скачайте архив из раздела Releases справа на странице.
2. Найдите папку игры. В ней есть папка rotwk, например: F:\games\AgeoftheRing\rotwk.
3. Распакуйте архив прямо в папку rotwk. В ней появятся файл winmm.dll и папка scripts.
4. Запустите игру как обычно, через лаунчер Age of the Ring.
5. При запуске вы услышите «AotR Access загружен».

Если лаунчер обновлял игру, проверьте, что winmm.dll и папка scripts на месте. Лаунчер может удалить незнакомые ему файлы. Тогда распакуйте архив ещё раз.

## Удаление

Удалите из папки rotwk файл winmm.dll, файл scripts\AotRAccess.asi и папку scripts\AotRAccess.

## Основные клавиши

В меню:

- Стрелки: двигаться по пунктам.
- Enter: нажать.
- Tab: следующая группа пунктов.

В бою:

- Page Down и Page Up: следующий и предыдущий объект в сканере.
- Ctrl+Page Down и Ctrl+Page Up: категория сканера.
- Shift+Page Down и Shift+Page Up: подкатегория.
- Home: повторить текущий объект.
- Shift+Home: что это за объект.
- End: состояние выделенного отряда.
- Ю (точка): выделить текущий объект.
- Ctrl+Home: камера к объекту.
- Ctrl+End: приказ на объект: идти, атаковать, войти.
- Alt+End: идти к объекту с боем.
- Ctrl+Shift+End: маршрут к цели миссии.
- Ctrl+Shift+Home: сводка угроз.
- Э (апостроф): режим команд. В нём Page Down и Page Up листают кнопки, Enter нажимает.
- Enter и Backspace: левый и правый щелчок в центре экрана.
- Равно и минус: камера ближе и дальше.
- Ctrl+Shift+F9: меню тренировки.

## Журнал

Мод пишет журнал в rotwk\scripts\AotRAccess.log. Если что-то не так, приложите этот файл к сообщению об ошибке.

## Сборка из исходников

- Нужен Visual Studio с инструментами C++ для x86.
- build.ps1 собирает build\AotRAccess.asi.
- deploy.ps1 собирает и копирует мод в игру. Путь к игре записан в начале файла.
- make_release.ps1 собирает архив релиза в папку release.
- Папка tools: скрипты на Python, которые читают карты и файлы игры и готовят данные в папке data.
- NOTEBOOK.md: рабочий журнал проекта: что доказано, что нет, история тестов.

## Лицензия

MIT, см. LICENSE. Prism распространяется по лицензии MPL-2.0, Ultimate ASI Loader по лицензии MIT.

Age of the Ring принадлежит команде Age of the Ring. «Битва за Средиземье 2» принадлежит Electronic Arts. Этот проект с ними не связан.

---

# AotR Access (English)

A mod that makes Age of the Ring playable for blind players: the game speaks through your screen reader.

## What it is

- Age of the Ring is a free mod for The Lord of the Rings: The Battle for Middle-earth II: The Rise of the Witch-king, installed with one installer from ageofthering.net.
- AotR Access hooks into the game and speaks it through your screen reader with the Prism library. Tested with NVDA.
- Speech is in Russian for now. Translations are welcome.
- This is an early test version. Please report problems in Issues.

## Features

- Menus read aloud: main menu, campaigns, skirmish, options, saves, dialogs.
- An object scanner in battle: what is around you, with distance and direction, by category.
- Campaign objectives with step-by-step places to go. Much of the Lord of the Rings campaign has been played through.
- Orders without the mouse: move, attack, attack-move, walk a route to the objective.
- Command mode: buttons of the selected unit or building, spell book, spell store.
- Battle reports: where the enemies are and how many.
- War of the Ring map: turns, armies, buildings (partly).
- A training menu for newcomers: money, command points, immortal own units.

## Requirements

- Age of the Ring 10.0.2 (standalone install with its own launcher).
- Windows and a screen reader (NVDA recommended).
- Proven on game version 2.01 (rotwk\game.dat). A game update may switch the mod off until it is updated.

## Install

1. Download the archive from Releases.
2. Find the game folder with the rotwk folder inside, for example F:\games\AgeoftheRing\rotwk.
3. Extract the archive straight into rotwk: winmm.dll and a scripts folder appear there.
4. Start the game through the Age of the Ring launcher as usual.
5. You hear "AotR Access загружен" at start.

After a launcher update, check that winmm.dll and the scripts folder are still there; extract again if not.

## Uninstall

Delete winmm.dll, scripts\AotRAccess.asi and the scripts\AotRAccess folder from rotwk.

## Log

The mod writes rotwk\scripts\AotRAccess.log. Attach it to bug reports.

## Building

Visual Studio with the x86 C++ tools; build.ps1 builds build\AotRAccess.asi, deploy.ps1 builds and copies into the game (game path at the top), make_release.ps1 makes the release archive. tools\ holds Python scripts that read the game's maps and files and produce data\. NOTEBOOK.md is the project's working log.

## Licence

MIT, see LICENSE. Prism is MPL-2.0, Ultimate ASI Loader is MIT. Age of the Ring belongs to the Age of the Ring team, The Battle for Middle-earth II to Electronic Arts; this project is not affiliated with either.
