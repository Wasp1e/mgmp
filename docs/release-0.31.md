# mgmp 0.31 pre-release

Windows x64; Mewgenics 1.1.21239 (Steam); network protocol 121.
Both players must update and restart the game.

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

Validation: 17 automated tests passed. The user confirmed the collars-to-gear
transition and backpack map blocking in a live session. Shelf refresh, furniture
modal transitions and other changes still need broader live validation.

## По-русски

- Совместное подтверждение ошейников и экипировки.
- Блокировка самого клика по тайлу, пока у игрока открыт рюкзак.
- Исправлен конфликт обработчиков окна инвентаря: клиент снова переходит
  от ошейников к экипировке, карта разблокируется после закрытия рюкзака.
- Обновление полок домашнего магазина после покупок хоста.
- Исправления синхронизации инвентаря, переходов к NPC и ночных событий.
- Синхронизация музыки и расширенные отчёты об остановках/логи друга.

Переход после ошейников и блокировка карты проверены пользователем.
Остальным изменениям требуется дополнительная проверка в совместной игре.
