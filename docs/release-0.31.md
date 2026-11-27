# mgmp 0.31 pre-release

Windows x64; Mewgenics 1.1.21239 (Steam); network protocol 121.
Both players must update and restart the game.

Updated November 28, 2026 (replaces the previous 0.31 download):

- Fixed stale enemy sprite facing after movement, including a retry once turn
  animations finish. The sprite follows combat facing without changing it.
- Backpack-blocked map clicks still meow for both players, without storing a
  vote or starting a delayed transition when the backpack closes.
- The client sees a paper note when the host leaves.
- Pre-entry saves are captured before selecting/walking to the next node, so
  a newly interrupted fight can be reloaded from the preceding map tile.
  Existing saves already stuck on an unfinished node are not migrated.
- RNG recording preserves the native randfloat register behavior. This repairs
  an instrumentation defect; the cause of the reported TallBotSuplex Halt
  remains unconfirmed.

- Shared confirmation for collars and gear, with synchronized party data.
- Map clicks are blocked before selection while a player has the backpack open.
- Fixed competing inventory screen hooks that prevented the client's transition
  from collars to gear and could leave map movement blocked.
- Client House shop shelves refresh when host stock changes.
- House inventory replacement waits for local UI to release item references.
- Replicated NPC office visits wait for local House modals to close.
- Improved shared night playback and daytime menu teardown.
- Added synchronized music and expanded peer log/halt reporting.
- Additional cat movement, ownership and save synchronization fixes.

Validation: 21 automated tests passed, including tests using the installed
game's native RNG code. The user reported no enemy-facing mismatch in the latest
session and provisionally accepted that fix. The collars-to-gear transition and
backpack map blocking were also confirmed in live play. Other new changes and
the TallBotSuplex Halt still require further live investigation.

## По-русски

Обновление от 28 ноября 2026 года заменяет предыдущую сборку 0.31:

- Исправлено расхождение спрайта врага и направления взгляда после движения.
  Последняя совместная игровая проверка прошла без замеченных расхождений.
- При открытом рюкзаке клик по тайлу сначала мяукает у обоих игроков, затем
  блокируется; отложенный переход после закрытия рюкзака не создаётся.
- У клиента появляется записка «Хост вышел».
- Сохранение перед входом в бой создаётся до выбора следующего тайла.
  Уже застрявшие сохранения автоматически не исправляются.
- Исправлено сохранение регистров при записи RNG. Связь этого дефекта с
  последним Halt после TallBotSuplex пока не доказана.
- Все 21 автоматический тест прошёл. Обоим игрокам заменить DLL и перезапустить игру.

- Совместное подтверждение ошейников и экипировки.
- Блокировка самого клика по тайлу, пока у игрока открыт рюкзак.
- Исправлен конфликт обработчиков окна инвентаря: клиент снова переходит
  от ошейников к экипировке, карта разблокируется после закрытия рюкзака.
- Обновление полок домашнего магазина после покупок хоста.
- Исправления синхронизации инвентаря, переходов к NPC и ночных событий.
- Синхронизация музыки и расширенные отчёты об остановках/логи друга.

Переход после ошейников и блокировка карты проверены пользователем.
Остальным изменениям требуется дополнительная проверка в совместной игре.
