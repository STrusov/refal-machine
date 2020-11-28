/**\file
 * \brief Интерфейс стандартной библиотеки РЕФАЛ.
 *
 * \addtogroup library Библиотечные функции РЕФАЛ-машины.
 *
 * В базовую библиотеку входят функции, которые затруднительно выразить на РЕФАЛ.
 *
 * Часть поля зрения, обрабатываемая функцией, называется _подвыражением_.
 * _Активное подвыражение_ включает в себя имя функции.
 *
 * Реализации функций принимают параметрами:
 * - адрес объекта с состоянием РЕФАЛ-машины;
 * - номер ячейки, предшествующей начальной обрабатываемого подвыражения;
 * - номер ячейки, следующей за конечной ячейкой обрабатываемого подвыражения.
 *
 * Описания функций заимствованы из книги Турчина В.Ф. (семантика с изменениями).
 * Нотация из Refal-05 Коновалова Александра.
 *
 * \{
 * \defgroup library-io    Функции ввода-вывода.
 * \defgroup library-math  Арифметические функции.
 * \defgroup library-str   Обработка символов и строк.
 * \defgroup library-rt    Системные функции.
 */

#include <assert.h>
#include "refal.h"

#pragma once

/**
 * Максимальное количество файловых дескрипторов,
 * поддерживаемых встроенными функциями классического РЕФАЛ-5.
 */
#define REFAL_LIBRARY_LEGACY_FILES 40

/**\addtogroup library-aux Вспомогательные функции.
 * Не вызываются из РЕФАЛ-програм непосредственно.
 * \{
 */
enum { refal_library_size = 24 };

extern
const struct refal_import_descriptor library[refal_library_size + 1];

static inline
int refal_library_call(
      rf_vm    *vm,
      rf_index prev,
      rf_index next,
      rf_index ordinal)
{
   assert(ordinal < refal_library_size);
   return library[ordinal].function(vm, prev, next);
}

/**\}*/
/**\}*/

rf_function  Card;
rf_cfunction Print;
rf_function  Prout;
rf_function  Open;
rf_function  Close;
rf_function  Get;
rf_function  Put;
rf_function  Putout;

rf_function  Add;
rf_function  Sub;
rf_function  Mul;
rf_function  Div;
rf_function  Mod;
rf_function  Compare;

rf_function  Type;
rf_function  Numb;
rf_function  Symb;
rf_function  Chr;
rf_function  Ord;

/**\addtogroup library-io
 * \{
 */

/**
 * Возвращает следующую строку из входного файла.
 * Если ввод производится из файла, при достижении конца файла возвращается
 * макро-цифра 0 (строк больше нет). При считывании с терминала тот же эффект
 * вызывает ввод комбинации Ctrl+D (Ctrl+Z в Windows).
 *
        <Card> == s.CHAR* 0?
*/
int Card(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Вывод подвыражения в стандартный поток вывода с переводом строки.
 * Сохраняет подвыражение в поле зрения.
 *
        <Print e.Expr> == e.Expr
 */
int Print(const rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Вывод подвыражения в стандартный поток вывода с переводом строки.
 * Удаляет подвыражение из поля зрения.
 *
        <Prout e.Expr> == []
 */
int Prout(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Открывает файл e.FileName и связывает его с файловым дескриптором s.FileNo.
 * s.Mode является одним из символов: 'r' (открыть для чтения), 'w' (открыть для
 * записи; если файл существует содержимое удаляется), либо 'a' (открыть для
 * записи в конец файла).
 * Дескриптор файла является целым числом в диапазоне 1…39
 * \see REFAL_LIBRARY_LEGACY_FILES.
 *
 * Если текущим режимом является чтение, а файл не существует, выдаётся ошибка.
 *
 * TODO указанная конвенция малопригодна для практических применений.
 *
        <Open s.Mode s.FileNo e.FileName> == []
        s.Mode ::=
            'r' | 'w' | 'a'
 */
int Open(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Закрывает соответствующий дескриптору s.FileNo файл.
 *
        <Close s.FileNo> == []
 */
int Close(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Действует подобно Card, за исключением того, что получает данные из файла,
 * указанного s.FileNo.
 *
        <Get s.FileNo> == s.Char* 0?
        s.FileNo ::= s.NUMBER
 */
int Get(rf_vm *restrict vm, rf_index prev, rf_index next);

int Put(rf_vm *restrict vm, rf_index prev, rf_index next);

int Putout(rf_vm *restrict vm, rf_index prev, rf_index next);

/**\}*/

/**\addtogroup library-math
 * \{
 */

/**
 * Возвращает сумму операндов (2-х s-переменных).
 *
       <Add s.NUMBER s.NUMBER> == s.NUMBER
*/
int Add(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Вычитает 2-ю s-переменную из 1-й в возвращает разность.
 *
       <Sub s.NUMBER s.NUMBER> == s.NUMBER
*/
int Sub(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Возвращает произведение операндов (2-х s-переменных).
 *
       <Mul s.NUMBER s.NUMBER> == s.NUMBER
*/
int Mul(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Возвращает частное от деления 1-й s-переменной на 2-ю, или 0 при ошибке деления.
 *
       <Div s.NUMBER s.NUMBER> == s.NUMBER
*/
int Div(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Возвращает остаток от деления 1-й s-переменной на 2-ю, или 0 при ошибке деления.
 *
       <Mod s.NUMBER s.NUMBER> == s.NUMBER
*/
int Mod(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 *  Сравнивает два числа и возвращает '-', когда s.X меньше, чем s.Y;
 *  '+', когда больше; '0', когда равны.
 *
       <Compare s.X s.Y>
         == '-' | '0' | '+'
        s.X, s.Y ::= s.NUMBER
 */
int Compare(rf_vm *restrict vm, rf_index prev, rf_index next);

/**\}*/

/**\addtogroup library-str
 * \{
 */

/**
 * Возвращает s.Type s.SubType e.Expr, где e.Expr остаётся неизменным, а
 * s.Type и s.SubType зависят от типа первого элемента выражения e.Expr:
 * - 'Lu'   заглавная латинская буква
 * - 'Ll'   строчная латинская буква
 * - 'P0'   печатный символ
 * - 'O0'   остальные символы
 * - 'D0'   десятичная цифра
 * - 'Wi'   идентификатор (функция). В книге 'F'.
 * - 'N0'   число
 * - 'B0'   скобка (открывающая)
 * - '*0'   пустое выражение
 *
 * TODO доработать для Unicode.
 */
int Type(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Возвращает макро-цифру, представленную строкой в поле зрения.
 * Если аргумент не начинается с последовательности цифр, функция возвращает 0.
 *
       <Numb s.Digit* e.Skipped> == s.NUMBER
       s.Digit ::= '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9'
*/
int Numb(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Является обратной к функции Numb. Возвращает строку десятичных цифр,
 * представляющую s.NUMBER. В данной реализации тип чисел знаковый.
 *
       <Symb s.NUMBER> == s.CHAR+
 */
int Symb(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Замещает всякое число в e.Expr литерой с соответствующим кодом.
 *
       <Chr e.Expr> == e.Expr’
 */
int Chr(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Замещает всякую литерой в e.Expr её кодом в UCS-4 (UTF-32).
 *
       <Ord e.Expr> == e.Expr’
 */
int Ord(rf_vm *restrict vm, rf_index prev, rf_index next);

/**\}*/

/**\addtogroup library-rt
 * \{
 */

/**
 *  Возвращает аргумент командной строки, который имеет порядковый номер s.N.
 *
       <Arg s.N>

 * Не реализована (требует сохранять состояние в статических переменных,
 * не ясно, как быть в случае встраивания интерпретатора).
 *
 * В программах РЕФАЛ рекомендуется использовать точку входа main или Main.
 * Эти функции получают в поле зрения список аргументов как (e.Arg)+
 *
 * В случае встраивания, аргументы размещаются `rf_alloc_strv()`.
 */
int Arg(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Ищет в поле зрения элемент, являющийся идентификатором вычислимой функции,
 * после чего применяет определённую им функцию к выражению e.Arg1 e.Arg2.
 *
       <Mu e.Arg1 s.Func e.Arg2> == <s.Func e.Arg>

  Функция реализована непосредственно в интерпретаторе.
*/
int Mu(rf_vm *restrict vm, rf_index prev, rf_index next);

/**\}*/

/**\addtogroup library-aux
 * \{
 */

/**
 * Отображает файл в память.
 *
 * \retval size Размер содержимого.
 * \result Указатель на начало файла либо \c MAP_FAILED при ошибке.
 */
static inline
void *mmap_file(
      const char  *name,   ///< Имя файла.
      size_t      *size)   ///< Указатель для сохранения результата.
{
   void *addr = MAP_FAILED;
   const int fd = open(name, O_RDONLY);
   if (fd >= 0) {
      struct stat sb;
      if (fstat(fd, &sb) >= 0) {
         *size = sb.st_size;
         addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      }
      close(fd);
   }
   return addr;
}

/**\}*/
