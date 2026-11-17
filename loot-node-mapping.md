# Маппинг LootNode: проверенные поля и координаты

## Результат

Проверены текущие `GameAssembly.dll` и `global-metadata.dat`, а также предоставленный дамп `ROB RUN_20261108_193627`. Перечисленные ниже смещения совпадают. В проверенных классах узлов нет `donorId`, `donor_id`, `name_ru`, `building`, `building_ru`, `district` или `district_ru`. Классы с точными именами `LootNodeDefinition`, `LootNodePreset` и `DonorNode` не найдены. `LootNodeData` существует, но содержит состояние обыска.

`map-points.csv` содержит 927 уникальных ключей вида `donor_159`. В нём нет игровых EntityId, координат или таблицы соответствия. Связь `donor_N → EntityId=N` либо `Index=N` не подтверждена. Автоматически назначать эти значения нельзя.

В живом снимке получены **58 узлов с валидными координатами**, 58 различных runtime ID и 53 различных строковых ID описания. Например:

| Runtime ID | Description.Id | Description.Index | X | Y | Z |
|---|---|---|---|---|---|
| 1140 | `0` | 0 | -481.659698 | 1.960000 | 5.242800 |
| 326 | `0` | 0 | 219.902405 | 11.307700 | 251.660706 |
| 1167 | `27` | 27 | 259.488892 | 10.400000 | 46.008099 |

Это подтверждает, что ID описания сам по себе не уникален среди экземпляров текущего снимка. Устойчивость runtime ID между матчами не установлена. Для ручного сопоставления нужны координаты и проверка места в игре.

## LootNodeView и прямые ссылки

`Game.Locations.LootNodeView : MonoBehaviour`, TypeDefIndex 5468:

| Смещение | Поле | Тип |
|---|---|---|
| `0x20` | `FullnessState` | `StateElementBase` |
| `0x28` | `TargetObjectView` | `TargetObjectView` |
| `0x30` | `AnimationView` | `ContainerAnimationView` |
| `0x38` | `_lightedView` | `LightedView` |

На самом `LootNodeView` нет прямого поля модели или описания узла.

Поля непосредственно связанных классов:

| Класс | Поля |
|---|---|
| `StateElementBase` | `0x20 _state: int`, `0x24 _isLocked: bool` |
| `TargetObjectView` | `0x20 _type: SubsystemType`, `0x28 _hintTransform: Transform`, `0x30 _additionalHintTransforms: Transform[]`, `0x38 _pingMarkerPositionTransform: Transform`, `0x40 _colliders: Collider[]`, `0x48 _unityRaycastTargetObjectOptions`, `0x50 _unitySpherecastTargetObjectOptions`, `0x58 _interactAreasTargetObjectOptions`, `0x60 _targetHintObject`, `0x68 _hintTransforms` |
| `ContainerAnimationView` | `0x20 _tween: TweenGroup`, `0x28 _hasAnimation: bool` |
| `LightedView` | `0x20 _rendererList: Renderer[]`, `0x28 _instantiated: bool`, `0x29 _value: Nullable<bool>` |

Связь представления с моделью хранится в `Game.Battle.Entities.Building.LootNodeStateUpdater`: `0x20 _view: LootNodeView`, `0x28 _lootNode: LootNodeModel`, `0x30 _small: int`, `0x34 _half: int`. Для текущей DLL: `Init` RVA `0xBA1FB0`, `OnUpdate` RVA `0xBA2030`, `GetState` RVA `0xBA1F10`. У `LootNodeEntityModel` поле `ServerModel: LootNodeModel` находится по `0x18`.

## Модель и описание с мировыми координатами

Рабочая цепочка уже доступна через реестр моделей:

```text
BattleEntitiesModel +0x60 → WorldEntitiesModel
WorldEntitiesModel +0xB0 → _lootNodes dictionary
entry.key                  = runtime EntityId
entry.value                = LootNodeModel*
LootNodeModel +0x10         → LocationLootNodeDescription*
```

`Models.Models.LootNode.LootNodeModel`, TypeDefIndex 3787:

| Смещение | Поле | Тип |
|---|---|---|
| `0x10` | `Description` | `LocationLootNodeDescription` |
| `0x18` | `StepsModel` | `LootNodeStepsModel` |
| `0x20` | `UnlockState` | `IPrimitiveModel<UnlockStateDescription>` |
| `0x28` | `ResetModel` | `LootNodeResetModel` |
| `0x30` | `IsEmptyModel` | `IPrimitiveModel<bool>` |

`Models.References.LootNode.LocationLootNodeDescription : MyEnumItem`, TypeDefIndex 1885:

| Смещение | Поле | Тип |
|---|---|---|
| `0x10` | `Id` — унаследовано | `string` |
| `0x18` | `Index` — унаследовано | `int` |
| `0x20` | `EntityId` | `int` |
| `0x28` | `LootNodeDescriptionFunc` | `Func<LootNodeDescription>` |
| `0x30` | `LootNodeLootDescriptionFunc` | `Func<LootNodeLootDescription>` |
| `0x38` | `Position` | `Vector3`, мировая позиция |
| `0x44` | `Orientation` | `Quaternion` |
| `0x54` | `InteractPoint` | `Vector3`, точка взаимодействия |
| `0x60` | `InteractIgnoreStaticBodies` | `HashSet<PhysicsBodyId>` |
| `0x68` | `InteractDescription` | `InteractDescription` |
| `0x70` | `SecurityAlarmDescriptionFunc` | `Func<LocationSecurityAlarmDescription>` |
| `0x78` | `SpawnProbability` | `float` |
| `0x7C` | `IsSpawnGuaranteed` | `bool` |
| `0x80` | `AvailableForUseRolesLazy` | `Lazy<HashSet<PlayerRoleDescription>>` |

В проверенном снимке `Description.EntityId` совпал с ключом словаря для всех 58 узлов. `Id` и `Index` — отдельные значения. Для координат не требуется искать нативный Transform.

## Остальные найденные описания

| Класс | Поля помимо унаследованных |
|---|---|
| `LootNodeDescription : MyEnumItem` | `0x20 IsLocked: bool`, `0x24 CompletelyLootedCumulativePoints: int` |
| `LootNodeLootDescription : MyEnumItem` | `0x20 StepsAmountFrom: int`, `0x24 StepsAmountTo: int`, `0x28 StepItemAmountFrom: int`, `0x2C StepItemAmountTo: int`, `0x30 ItemFunc: Func<InventoryItemDescription>` |
| `LootNodeData : CompositeData` | `0x20 Steps: IPrimitiveData<int>`, `0x28 UnlockState`, `0x30 IsEmpty: IPrimitiveData<bool>` |
| `LootNodeStepsModel` | `0x10 _stepsData`, `0x18 _description: LocationLootNodeDescription`, `0x20 _totalSteps` |
| `LootNodeResetModel` | `0x10 _steps`, `0x18 _unlockState`, `0x20 _isEmpty`, `0x28 _lootNodeDescription` |
| `LootNodeViewDescription` | `0x10 Identified: IdentifiedProperty<LootNodeDescription>`, `0x18 Name: LocalizedString`, `0x20 ActionTip: LocalizedString`, `0x28 IsSmallTarget: bool`, `0x30 UnlockSwitch: Switch`, `0x38 OpenAfterUnlockEvent: Event` |

`LootNodeViewDescription.Name` — найденная возможность получить локализованную подпись типа узла. Это `LocalizedString`, а не готовая строка `name_ru`. В этом классе нет привязки к зданию, району или donor-ID. Сам факт наличия общего имени не устанавливает соответствие отдельной точке CSV.

## Что добавлено в проект

`game_entities.h` содержит отдельную структуру `LootNode` и `GameEntities::loot_nodes()`. В структуре сохраняются runtime ID, адрес модели и описания, строковый ID и индекс описания, координаты, валидность и источник координат. Поля `donor_id`, `name_ru`, `building_ru`, `district_ru` оставлены пустыми: в дампе их источник не найден.

`game_entities.cpp` собирает узлы из `_lootNodes` при штатном обновлении этого реестра. Проверяются класс модели, класс описания, чтение координат и совпадение `Description.EntityId` с ключом словаря. Нулевой указатель или ошибка чтения пропускают кандидата. Изменившийся словарь не публикуется. Нулевая позиция `(0,0,0)` допустима для диагностического экспорта. При невалидном `Position` используется `InteractPoint`; если обе позиции невалидны, идентификаторы сохраняются с `position_valid=false`.

Лог выводится при обнаружении или изменении узла, а не каждый кадр:

```text
[LOOT-NODE] runtime_id=1167 definition_id=27 index=27 position=(259.488892,10.400000,46.008099) valid=1 source=Position+0x38 model=0x... definition=0x... donor_id=unmapped
```

Новый вектор передаётся вместе со снимком через `swap_frame`. Названия предметов, оружия, фильтры и CSV-каталоги этим изменением не затрагивались.

## Файл для ручного сопоставления

`loot-node-runtime-positions.csv` — разовая read-only выгрузка текущего снимка. Она содержит UTF-8 BOM и столбцы:

```text
runtime_id,definition_id,definition_index,x,y,z,position_valid,source,donor_id,name_ru,building_ru,district_ru
```

Можно заполнить последние четыре столбца, сопоставив координаты с конкретными зданиями и строками `map-points.csv`. Это подготовка таблицы соответствия: загрузка заполненного файла в оверлей сейчас не реализована. Чтобы переносить соответствия между матчами, сначала нужно проверить устойчивость координат и не полагаться только на runtime ID или повторяющийся `definition_id`.

Исходник разовой пробы: `work/loot-dump/node_mapping_probe.cpp`. Она только открывает уже доступное устройство и читает память; драйвер и игру не запускает. На момент выгрузки: `exported=58 runtime_entries=58 unresolved=0`. Эта выгрузка не подтверждает наличие всех 927 CSV-точек в текущем игровом состоянии.

## Источники

- Текущий дамп: `work/loot-dump/result/dump.cs`. `LocationLootNodeDescription` начинается со строки 86724, `LootNodeModel` — 171394, `LootNodeData` — 215800, `LootNodeView` — 236537, `LootNodeEntityModel` — 346914, `LootNodeStateUpdater` — 346963, `LootNodeViewDescription` — 347042.
- Пользовательский дамп: `C:/Users/lucky/Desktop/ROB RUN_20261108_193627/offsets.json` и `CppSDK/Models.hpp`.
- SHA256 текущей DLL: `03003754353FF08380DC8E91920F2C77F7977B370D413083ACFE9B4BC65C18AB`.
- SHA256 текущего metadata: `23BED54126A2B89E9C78CFDBD59DC3BE2BC14A5817823CF057CDE33984618CC9`.
- Release MSVC x64: `C:/Users/lucky/Documents/opencode1/build/vs2026-scarlex-node-mapping-x64/Release/rob_run_tool.exe`.
- Синтетическая проверка: `PASS: 486 checks including fixture bounds`, включая 11 новых проверок маппинга: раздельные ID, допустимость нулевой позиции, fallback, недоступные позиции, несовпадение владельца, публикация, перенос снимка, очистка реестра и нулевое описание.
