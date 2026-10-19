// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Профиль DOOMEternalx64vk.exe версии 1.1: все адреса (RVA от начала образа exe в памяти) и смещения
// полей, которые collision-viewer читает из памяти игры. Больше нигде в коде чисел раскладки нет.
//
// Откуда они: раскладки классов движка восстановлены автором viewer по отражению (reflection) SDK
// и самой 1.1; раскладки Havok (hknp*) совпадают с описаниями типов TAG0, встроенными в карты.
// Проверены на exe с SHA-256 0fa3177e…ea7f (Steam, ветка 1.1). Для другой версии exe они неверны:
// поэтому до любых чтений сверяется заголовок PE и сигнатура кода (version_check.h).

#pragma once

#include <cstddef>
#include <cstdint>

namespace doomfix::cv::exe11 {
inline uintptr_t kResolvedRenderer=0, kResolvedGameSystem=0, kResolvedConsole=0;
inline uintptr_t kResolvedView=0;

// --- Опознание exe ------------------------------------------------------------------------------------

// Заголовок PE: IMAGE_FILE_HEADER::TimeDateStamp и IMAGE_OPTIONAL_HEADER64::SizeOfImage.
constexpr uint32_t kTimeDateStamp = 0x5ec7deca;
constexpr uint32_t kSizeOfImage = 0x1d551000;
// Первые 10 байт функции idCVar::SetString (mov [rsp+8],rbx; push rdi; sub rsp,20h). Исходный
// viewer проверял их при каждом чтении камеры; здесь — один раз при старте фичи.
inline uintptr_t kSetStringRva = 0x339b10;
constexpr uint8_t kSetStringBytes[10] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20};

// --- Камера (рендерер) ----------------------------------------------------------------------------

inline uintptr_t kRendererRva = 0x5bd6540;      // указатель на рендерер
inline uintptr_t kRendererViews = 0x5a7188;     // рендерер: указатель на массив указателей на виды
inline uintptr_t kRendererViewCount = 0x5a7190; // рендерер: int, число видов (берётся вид 0)
constexpr int kMaxViews = 32;
inline uintptr_t kViewPosition = 0x20a28;       // вид: float[3], позиция камеры в мире
inline uintptr_t kViewMatrix = 0x213f0;         // вид: float[16], матрица мир -> clip по строкам

// --- idGameSystemLocal, карта, игрок ------------------------------------------------------------------

inline uintptr_t kGameSystemRva = 0x33fc468;    // указатель на idGameSystemLocal
inline uintptr_t kGameSystemState = 0x44;       // uint32: 2 — идёт уровень (иначе меню/загрузка)
constexpr uint32_t kStateInLevel = 2;
inline uintptr_t kGameSystemMap = 0x50;         // указатель на текущую карту (idMapInstance)

inline uintptr_t kMapName = 0x20;               // карта: char*, имя ("game/sp/e1m1_intro/...")
inline uintptr_t kMapNameLength = 0x28;         // карта: uint32, длина имени
constexpr uint32_t kMaxMapNameLength = 511;
inline uintptr_t kMapPlayer = 0x1ad0;           // карта: idPlayer* игрока
inline uintptr_t kMapEntities = 0x61cb0;        // карта: EntityEntry*, список сущностей
inline uintptr_t kMapEntityCount = 0x61cb8;     // карта: uint32, длина списка
constexpr uint32_t kMaxEntities = 32768;

// Элемент списка сущностей карты.
struct EntityEntry {
  uint32_t id;
  uint32_t generation;
  uintptr_t entity;    // idEntity*
  uintptr_t previous;
  uintptr_t next;
};
static_assert(sizeof(EntityEntry) == 32, "Layout of the 1.1 entity list");

inline uintptr_t kPlayerVtableRva = 0x2984578;  // vtable idPlayer (проверка, что указатель — игрок)
inline uintptr_t kEntityRemoved = 0xa4;         // idEntity: uint8, сущность удалена
inline uintptr_t kEntityPhysics = 0x1d0;        // idEntity: указатель на физику (idPhysics_*)
constexpr size_t kEntityReadSize = 0x240;          // сколько байт idEntity читать за раз

// HUD игрока: пауза и открытые меню.
inline uintptr_t kPlayerHud = 0x36020;          // idPlayer: встроенный idHUD
inline uintptr_t kHudPaused = 0x3ac;            // idHUD: uint8, пауза
inline uintptr_t kHudMenus = 0x358;             // idHUD: idMenu**, массив меню
inline uintptr_t kHudMenuCount = 0x360;         // idHUD: uint32, число меню
constexpr uint32_t kMaxMenus = 64;
constexpr size_t kMenuReadSize = 0xc8;             // сколько байт idMenu читать
inline uintptr_t kMenuPopups = 0x80;            // idMenu: int, открытые всплывающие окна
inline uintptr_t kMenuScreen = 0x90;            // idMenu: int, текущий экран (-1 — нет)
inline uintptr_t kMenuNextScreen = 0x94;        // idMenu: int, следующий экран (переход)
inline uintptr_t kMenuHidden = 0xb1;            // idMenu: uint8, меню явно скрыто
inline uintptr_t kMenuBlocking = 0xb2;          // idMenu: uint8, меню перекрывает игру (диалог)

// --- Консоль (idConsoleLocal, встроена в синглтон движка) -------------------------------------------

inline uintptr_t kConsoleRva = 0x410bc50;
inline uintptr_t kConsoleVtableRva = 0x26080b8;
inline uintptr_t kConsoleFraction = 0x6080fc;   // float displayFrac: доля высоты экрана
inline uintptr_t kConsoleForced = 0x608108;     // uint8: консоль на весь экран

// --- Физика сущностей (idPhysics_*) ------------------------------------------------------------------

inline uintptr_t kDynamicEntityVtableRva = 0x27f2b58;  // vtable idDynamicEntity (кандидат в стены)
constexpr size_t kPhysicsReadSize = 0x208;
inline uintptr_t kPhysicsType = 0x14;           // uint32: 2 — статическая, 7 — твёрдое тело
constexpr uint32_t kPhysicsStatic = 2;
constexpr uint32_t kPhysicsRigidBody = 7;
inline uintptr_t kPhysicsContents = 0x44;       // uint32: флаги contents (ниже)
inline uintptr_t kPhysicsDisabled = 0x4f;       // uint8: физика выключена
inline uintptr_t kPhysicsHavok = 0x80;          // физика игрока: указатель на idHavokPhysics
inline uintptr_t kPhysicsBody = 0x88;           // uint32: id тела Havok (~0 — нет тела)
inline uintptr_t kPhysicsAxis = 0x8c;           // float[9]: оси (по столбцам)
inline uintptr_t kPhysicsOrigin = 0xb0;         // float[3]: положение
inline uintptr_t kPhysicsShape = 0x1c0;         // указатель на обёртку -> корневая форма hknp
inline uintptr_t kPhysicsInWorld = 0x210;       // uint8 (твёрдое тело): добавлено в мир Havok
inline uintptr_t kHavokWorldShape = 0x210b0;    // idHavokPhysics: обёртка -> корневая форма мира

// Флаги contents.
constexpr uint32_t kContentsSolid = 0x1;
constexpr uint32_t kContentsOpaque = 0x2;
constexpr uint32_t kContentsPlayerClip = 0x8;
constexpr uint32_t kContentsTrigger = 0x4000;
constexpr uint32_t kContentsNoCollision = 0x40000000;  // скрытые/выключенные сущности

// --- RTTI движка: имя класса по vtable ------------------------------------------------------------------
// Слот vtable +16 — метод GetType(): `lea rax, [rip+disp32]; ret` (48 8d 05 xx xx xx xx c3); по адресу
// rip+disp32 — описание типа, в его начале — char* имени класса. Триггеры — idTrigger* и
// idEncounterTrigger*.
inline uintptr_t kVtableTypeGetter = 16;
inline uintptr_t kMaxVtableRva = 0x7000000;     // vtable внутри образа exe
inline uintptr_t kMaxCodeRva = 0x2500000;       // код GetType() внутри образа exe

// --- Формы Havok (hknp*) -------------------------------------------------------------------------------------

constexpr size_t kShapeReadSize = 112;             // заголовок формы, который читается целиком
inline uintptr_t kShapeFlags = 0x20;            // uint16: бит 2 — выпуклый многогранник (и box)
constexpr uint16_t kShapeFlagConvex = 0x2;
inline uintptr_t kShapeType = 0x22;             // uint8: тип формы
inline uintptr_t kShapeUserData = 0x30;         // havokShapeData_t*, validated through its root table
inline uintptr_t kShapeDataRoot = 0x0;          // havokShapeData_t.rootShape -> idHavokShape
inline uintptr_t kShapeDataContents = 0x8;      // havokShapeData_t.contentFlags
inline uintptr_t kShapeDataList = 0x8;          // idHavokShape.shapeData (idList)
inline uintptr_t kShapeDataCount = 0x10;
constexpr size_t kShapeDataSize = 0x58;
constexpr uint8_t kShapeCompound = 13;             // hknpCompoundShape
constexpr uint8_t kShapeCompressedMesh = 9;        // hknpCompressedMeshShape

// hknpCompoundShape: массив экземпляров.
inline uintptr_t kCompoundInstances = 0x50;     // указатель на экземпляры
inline uintptr_t kCompoundCount = 0x58;         // uint32, число экземпляров
constexpr size_t kInstanceSize = 112;              // экземпляр: float[16] матрица по столбцам (+0),
inline uintptr_t kInstanceScale = 0x40;         //   float[3] масштаб,
inline uintptr_t kInstanceShape = 0x50;         //   указатель на дочернюю форму,
inline uintptr_t kInstanceDisabled = 0x5c;      //   uint8 isEmpty — свободный слот, не фильтр столкновений

// hknpConvexPolytopeShape: относительные массивы (uint16 число, uint16 смещение от поля числа).
inline uintptr_t kConvexVertices = 0x42;        // float[4] на вершину
inline uintptr_t kConvexFaces = 0x54;           // uint16 первый угол, uint8 число углов, uint8 —
inline uintptr_t kConvexIndices = 0x58;         // uint8 индексы вершин углов граней

// hknpCompressedMeshShape: данные (указатель +0x50), в них дерево (+0x20).
inline uintptr_t kMeshData = 0x50;
inline uintptr_t kMeshTree = 0x20;
inline uintptr_t kTreeDomain = 16;              // float[8]: min xyz(w), max xyz(w)
inline uintptr_t kTreeSections = 64;            // массивы hkArray: указатель + uint32 число
inline uintptr_t kTreePrimitives = 80;          //   uint8[4] — четырёхугольник (или 0xdeaddead)
inline uintptr_t kTreeSharedIndices = 96;       //   uint16
inline uintptr_t kTreePackedVertices = 112;     //   uint32: 11+11+10 бит
inline uintptr_t kTreeSharedVertices = 128;     //   uint64: 21+21+22 бит в домене дерева
inline uintptr_t kTreeDataRuns = 144;           // uint16 tag, uint8 first primitive, uint8 count
constexpr size_t kSectionSize = 96;                // секция:
inline uintptr_t kSectionCodec = 48;            //   float[6]: смещение xyz, шаг xyz
inline uintptr_t kSectionFirstPacked = 72;      //   uint32: первая упакованная вершина
inline uintptr_t kSectionShared = 76;           //   uint32: (первый общий индекс << 8) | ...
inline uintptr_t kSectionPrimitives = 80;       //   uint32: (первый примитив << 8) | число
inline uintptr_t kSectionDataRuns = 84;         // uint32: (first data run << 8) | count
inline uintptr_t kSectionPackedCount = 88;      //   uint8
inline uintptr_t kSectionSharedCount = 89;      //   uint8

}  // namespace doomfix::cv::exe11
