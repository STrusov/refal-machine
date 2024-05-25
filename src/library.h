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
 *
 */

#pragma once

#include "refal.h"

/**
 * Максимальное количество файловых дескрипторов,
 * поддерживаемых встроенными функциями классического РЕФАЛ-5.
 */
#define REFAL_LIBRARY_LEGACY_FILES 40

extern
const struct refal_import_descriptor library[];

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

rf_function  Push;
rf_function  Pop;

rf_function  Type;
rf_function  Numb;
rf_function  Symb;
rf_function  Chr;
rf_function  Ord;

rf_function  GetEnv;
rf_cfunction Exit;
rf_function  System;

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
int Card(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Вывод подвыражения в стандартный поток вывода с переводом строки.
 * Сохраняет подвыражение в поле зрения.
 *
        <Print e.Expr> == e.Expr
 */
int Print(const struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Вывод подвыражения в стандартный поток вывода с переводом строки.
 * Удаляет подвыражение из поля зрения.
 *
        <Prout e.Expr> == []
 */
int Prout(struct refal_vm *vm, rf_index prev, rf_index next);

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
int Open(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Закрывает соответствующий дескриптору s.FileNo файл.
 *
        <Close s.FileNo> == []
 */
int Close(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Действует подобно Card, за исключением того, что получает данные из файла,
 * указанного s.FileNo.
 *
        <Get s.FileNo> == s.Char* 0?
        s.FileNo ::= s.NUMBER
 */
int Get(struct refal_vm *vm, rf_index prev, rf_index next);

int Put(struct refal_vm *vm, rf_index prev, rf_index next);

int Putout(struct refal_vm *vm, rf_index prev, rf_index next);

/**\addtogroup library-aux Вспомогательные функции.
 * Не вызываются из РЕФАЛ-програм непосредственно.
 * \{
 * \}*/

/**\}*/

/**\addtogroup library-math
 * \{
 */

/**
 * Возвращает сумму операндов (2-х s-переменных).
 *
       <Add s.NUMBER s.NUMBER> == s.NUMBER
*/
int Add(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Вычитает 2-ю s-переменную из 1-й в возвращает разность.
 *
       <Sub s.NUMBER s.NUMBER> == s.NUMBER
*/
int Sub(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Возвращает произведение операндов (2-х s-переменных).
 *
       <Mul s.NUMBER s.NUMBER> == s.NUMBER
*/
int Mul(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Возвращает частное от деления 1-й s-переменной на 2-ю, или 0 при ошибке деления.
 *
       <Div s.NUMBER s.NUMBER> == s.NUMBER
*/
int Div(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Возвращает остаток от деления 1-й s-переменной на 2-ю, или 0 при ошибке деления.
 *
       <Mod s.NUMBER s.NUMBER> == s.NUMBER
*/
int Mod(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 *  Сравнивает два числа и возвращает '-', когда s.X меньше, чем s.Y;
 *  '+', когда больше; '0', когда равны.
 *
       <Compare s.X s.Y>
         == '-' | '0' | '+'
        s.X, s.Y ::= s.NUMBER
 */
int Compare(struct refal_vm *vm, rf_index prev, rf_index next);

/**\}*/

/**\addtogroup library-stack
 * \{
 */

/**
 *      <Br e.Name '=' e.Expr>
 * Закапывает выражение \c e.Expr под именем \c e.Name.
 * Имя не должно содержать знак \c '=' на верхнем уровне структуры.
 *
 * \c Push не использует \c '=' в качестве разделителя.
 * Ожидает в качестве имени имя функции, предложения которой
 * не содержат выражение-результат (то есть определены без =).
 * Имя можно указать как идентификатором, так и текстом.
 * Таким образом можно указать начальное содержимое и отчасти решается проблема
 * «чтобы понять, какими глобальными переменными пользуется некоторая функция,
 * нужно изучить исходный текст самой функции, а также других функций, которые
 * она вызывает» стр.6 http://refal.botik.ru/events/IPSRAN-MGTU-seminar-17-06-2023/RAYAP_Lecture4.pdf
 *
        <Push s.Func | s.CHAR* e.Expr> == []
*/
int Push(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 *      <Dg e.Name>
 * Выкапывает выражение, закопанное под именем e.Name, то есть возвращает последнее
 * выражение, закопанное под этим именем и удаляет его из стэка. Если не существует
 * выражения, закопанного под именем e.Name, Dg возвращает пустое выражение.
 *
 * \c Pop работает подобно; имя можно указывать как идентификатором, так и текстом.
 *
        <Pop s.Func | s.CHAR*> == e.Value
 */
int Pop(struct refal_vm *vm, rf_index prev, rf_index next);


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
int Type(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Возвращает макро-цифру, представленную строкой в поле зрения.
 * Если аргумент не начинается с последовательности цифр, функция возвращает 0.
 *
       <Numb s.Digit* e.Skipped> == s.NUMBER
       s.Digit ::= '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9'
*/
int Numb(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Является обратной к функции Numb. Возвращает строку десятичных цифр,
 * представляющую s.NUMBER. В данной реализации тип чисел знаковый.
 *
       <Symb s.NUMBER> == s.CHAR+
 */
int Symb(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Замещает всякое число в e.Expr литерой с соответствующим кодом.
 *
       <Chr e.Expr> == e.Expr’
 */
int Chr(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Замещает всякую литерой в e.Expr её кодом в UCS-4 (UTF-32).
 *
       <Ord e.Expr> == e.Expr’
 */
int Ord(struct refal_vm *vm, rf_index prev, rf_index next);

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
int Arg(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Ищет в поле зрения элемент, являющийся идентификатором вычислимой функции,
 * после чего применяет определённую им функцию к выражению e.Arg1 e.Arg2.
 *
       <Mu e.Arg1 s.Func e.Arg2> == <s.Func e.Arg>

  Функция реализована непосредственно в исполнителе.
*/
int Mu(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Возвращает в поле зрения значение переменной окружения с именем e.EnvName.
 *
 * TODO Реализация из Refal-05 не различает случаи, когда переменная определена
 * с пустым значением, или не определена.
 *
       <GetEnv e.EnvName> == e.EnvValue
       e.EnvName, e.EnvValue ::= s.CHAR*
 */
int GetEnv(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Завершает исполнение процесса вызовом `exit()`, возвращая числовое
 * значение s.RetCode.
 *
       <Exit s.RetCode>
 */
int Exit(const struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Выполняет команду e.Command при помощи командного процессора системы.
 *
 * Если e.Command пуста, определяет наличие командного процессора и возвращает
 * 0 в случае отсутствия.
 *
       <System e.Command> == e.RetCode
       e.Command ::= s.CHAR*
       e.RetCode ::= '-'? s.NUMBER
 */
int System(struct refal_vm *vm, rf_index prev, rf_index next);

/**\}*/
