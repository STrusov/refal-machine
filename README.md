# Рефал-М

## О языке РЕФАЛ

РЕкурсивных Формул АЛгоритмический является одним из старейший языков программирования.
Создал его в 60-х годах прошлого столетия [Валентин Фёдорович Турчин](https://computer-museum.ru/histussr/turchin_sorucom_2011.htm).
С тех пор вышел ряд отличающихся синтаксисом и семантикой версий. Интересующимся
рекомендуется ознакомиться со [сравнением](https://github.com/Mazdaywik/direct-link/blob/master/refal-compare.md),
которое составил ведущий разработчик компилятора [Рефал-5λ](https://github.com/bmstu-iu9/refal-5-lambda)
Александр Коновалов.

## О реализации

Рефал-машина предназначена для исполнения написанных на РЕФАЛ сценариев
(на данный момент поддерживается [Базисный РЕФАЛ](doc/%D0%A0%D0%95%D0%A4%D0%90%D0%9B-5%20%D0%A0%D1%83%D0%BA%D0%BE%D0%B2%D0%BE%D0%B4%D1%81%D1%82%D0%B2%D0%BE%20%D0%BF%D0%BE%20%D0%BF%D1%80%D0%BE%D0%B3%D1%80%D0%B0%D0%BC%D0%BC%D0%B8%D1%80%D0%BE%D0%B2%D0%B0%D0%BD%D0%B8%D1%8E%20%D0%B8%20%D1%81%D0%BF%D1%80%D0%B0%D0%B2%D0%BE%D1%87%D0%BD%D0%B8%D0%BA%20%D0%A2%D1%83%D1%80%D1%87%D0%B8%D0%BD%20%D0%92.%D0%A4.pdf), за исключением нескольких встроенных функций) в ОС Linux. Реализована на яыке Си и имеет минимум зависимостей (в том числе от стандартной библиотеки Си). Представляет собой (потенциально) встраиваемый исполнитель (интерпретатор), предварительно переводящий исходный текст в коды операций (опкоды), которые и исполняется. Трансляция выполняется в один проход, однако, поддерживается традиционный для РЕФАЛ произвольный порядок определения функций.

Основные отличия:
* Поддержка кириллицы (и практически любых Уникод-сомволов) в идентификаторах.
* Префиксы `s.` `t.` и `e.` переменных могут быть соответственно `?` `!` и `.` или `…`. Префикс является частью имени: переменные `s.x` и `?x` различаются (что не всегда очевидно, потому
лучше не смешивать префиксы).
* Начинающиеся с `$` директивы упразднены (добавлена [поддержка модулей](#поддержка-модулей)).
* Поддержаны функции из одного предложения, без блока `{ }`. В таковых неявно
добавляется вторым предложением ` = ;`, что разрешает однострочную запись
простых рекурсивных вызовов ("циклов").
* В вычислительных скобках `< >` имя вызываемой функции не обязательно должно
идти непосредственно после открывающей скобки, перед ним можно расположить данные
или простой идентификатор. Транслятор знает, что можно вызвать. Это же послабление
относится и к функции `Mu`, которая выполняет поиск вызываемой функции среди
аргументов во время выполнения (пропуская структурные скобки).
* Имя вызываемой функции можно указывать `Mu` в виде текстовой строки, в том числе
включающей предшествующее имя модуля, отделённое пробелом. При этом применимо
вышеуказанное правило: `Mu` рассмотрит строку целиком (до первого отличного
от буквы аргумента); если функция с таким именем существует и вычислима, она будет
вызвана, иначе принимается за аргумент вызываемой функции.
* Имена функций, не содержащих вычисляемых предложений (подобие [ENUM Рефал-05](https://github.com/Mazdaywik/Refal-05?tab=readme-ov-file#особенности-языка)),
можно использовать для «[копилки](#особенности-копилки)» . Произвольные имена «ящиков» не допускаются.
Функция `Push`, в отличие от `Br` Рефал-5, не требует символ `=` для разделения
имени «ящика» и данных. Идентификатор ящика ищется подобно `Mu` и
может располагаться среди прочего. Вместо `Dg` используется `Pop`.
* Помимо точки входа `Go` возможно использовать `Main`, которая принимает в поле
зрения аргументы командной строки (каждый заключён в структурные скобки, как в
результате `ArgList` из [LibraryEx](https://github.com/Mazdaywik/refal-5-framework/blob/master/docs/LibraryEx.md#функция-arglist)).
Кроме того, поддерживается точка входа `Начало` — в таком случае исполнитель
проверяет, содержит ли первое её предложение образец, надо ли передавать аргументы.
Допустимы вышеперечисленные имена, начинающиеся со строчной буквы — результат их
исполнения выводится, а не отбрасывается.

        // Допустимы комментарии в одну строку в стиле С++
        
        * Вариант классической РЕФАЛ-программы.
        
        Начало = <Палиндром? <удалить пробелы "я разуму уму заря    ">>
                 <Палиндром? <удалить пробелы "я иду съ мечемъ судия">>;
                                                     * Гавриил Державин
        
        Палиндром? {
           ?символ … ?символ = <Палиндром? …>;
           ?символ           = <Вывод "Палиндром">;
                             = <Вывод "Палиндром">;
                   …         = <Вывод "Остаток: "…>;
        }
        
        удалить {
           пробелы .символы " " .остаток = .символы <удалить пробелы .остаток>;
           пробелы … = …;
        }
        
        * явное определение не обязательно.
        пробелы;
        
        Вывод . = <Prout .>;

### Особенности «копилки»

Начальное содержимое можно задать в исходном тексте:

        ящик "данную строку получит функция <Pop ящик>";
        
        полный_ящик {
           "<Pop полный_ящик> снимет с верхушки стека это";
           "а при повторном вызове это"
        }

По сути такия ящики (назовём их «полными») являются функциями без результатного выражения и переменных
в образце.

Когда «полный» ящик встречается в выражении-образце, Поле Зрения сопоставляется
с его содержимым. При этом предложения ящика (если их больше одного) просматриваюся
по очереди согласно обычным для Рефал правилам (до первого совпадения).
При успешном сопоставлении участок ПЗ может быть присвоен последующей (связанной)
e-переменной, если она открытая.

        ноль "0";
        один "1";
        _2-e { ноль; один }
        _8-e { _2-e; "2"; "3"; "4"; "5"; "6"; "7"; }
        _10e { _8-e; "8"; "9" }
        _16e { _10e; "A"; "B"; "C"; "D"; "E"; "F"; }
        
        тип {
          _2-e . = . " - " _2-e;
          _8-e . = . " - " _8-e;
          _10e . = . " - " _10e;
          _16e . = . " - " _16e;
          .    = -;
        }
        
        очередь  ? . = (<тип ?>) "\t" <очередь .>;
        
        начало = <очередь "0123456789ABCDEFG">;

В случае закрытой e-переменной происходит повторное сравнение ПЗ с её значением
(и длина не обязательно должна совпадать).

### Поддержка модулей

Для вызова функций, реализованных в другой единице трансляции (модуле), следует
импортировать модуль явно. Для чего указывается имя модуля, завершаемое двоеточием:

        Модуль: функция1 функция2;

РЕФАЛ-машина ищет реализацию модуля в файлах `Модуль.реф` и `Модуль.ref`.
Указанные в команде импорта идентификаторы вносится в текущую область пространства
имён. Остальные идентификаторы модуля так же доступны, если их имя предварительно
квалифицировать именем модуля:

        * Импортируем идентификаторы тест1 и тест2 из файла "Модуль1.реф"
        Модуль1: тест1 тест2;
        go = <тест1> <тест2>
           * Вызов по полному имени.
             <Модуль1 тест3>;

Такми же образом можно получить доступ к функциям модуля, импортированного
импортируемым модулем. Если не возникнет сложностей с рекурсией.

Если же модулю требуются встроенные функции, то их придётся импортировать из
«глобального» пространства имён:

        : Print Prout;

Так же возможно импортировать и функции из основной программы, если они
определены раньше команды импорта.

Способность функции `Mu` принимать имя в виде текста позволяет вызывать
любые функции из всех модулей, какие были импортированы во время трансляции.
На данном этапе возможности сокрытия нет; поиск происходит в глобальном
пространстве имён, потому функциям модуля обязательно должно предшествовать его имя.
Не исключено, что поведение будет изменено в будущих версиях.

### Запуск исполнителя

Распознаются следующие ключи:
* `+v` Выводит версию.
* `-v` Выводит версию и завершает работу.
* `+w` Предупреждения выводятся (по умолчанию). В частности, для неявно определённых идентификаторов.
* `-w` Предупреждения не выводятся.
* `+n` Замечания выводятся. Создание копий e- и t-переменных может оказаться накладным.
* `-n` Замечания не выводятся (по умолчанию).

После ключей (если они есть) следует имя файла с программой на Рефал.
Может представлять собой символ `-` (минус) для чтения потока ввода.
Остальные аргументы командной строки передаются исполняемой программе (в функцию `Начало` или `Main`).
Несложно обеспечить возможность указать несколько файлов с исходным текстом
(вспомогательные, а следом главный, определяемый по наличию точки входа), но
пока не ясно, зачем такое надо.

### Совместимость

На текущем этапе интерпретатор способен [исполнять](examples/refal-05.sh) компилятор [Refal-05](https://github.com/Mazdaywik/Refal-05) после [адаптации](examples/refal-05.v3.1.patch) его исходных текстов.

### Производительность

Нижеприведённые данные не претендуют на результаты измерений (погрешность не определялась) и предоставлены в качестве грубой оценки.

Интерпретатор версии 0.1.4 собран командой `make clean all` при помощи gcc (Gentoo 10.3.0 p1) без оптимизации под архитектуру процессора. Этот же gcc используется при компиляции refal05c с -O2. Запуск на процессоре Zen+ 3,6ГГц.

#### Транспиляция Refal-05 (версия 3.1) самим собой в исходный текст на Си

Запуск под интерпретатором (лишний вывод вырезан)

        $ time R05CCOMP= refal src/refal05c.ref \
            Refal-05/src/refal05c \
            Refal-05/src/R05-AST  \
            Refal-05/src/R05-CompilerUtils \
            Refal-05/src/R05-Lexer \
            Refal-05/src/R05-Parser \
            Refal-05/src/R05-Generator \
            Refal-05/src/LibraryEx \
            Library refal05rts

        real    0m1,941s
        user    0m1,935s
        sys     0m0,003s

Запуск откомпилированного `refal05c`

        $ time R05CCOMP= refal05c \
            Refal-05/src/refal05c \
            Refal-05/src/R05-AST  \
            Refal-05/src/R05-CompilerUtils \
            Refal-05/src/R05-Lexer \
            Refal-05/src/R05-Parser \
            Refal-05/src/R05-Generator \
            Refal-05/src/LibraryEx \
            Library refal05rts

        real    0m0,598s
        user    0m0,196s
        sys     0m0,401s

#### Исполнение интерпретатора лямбда-исчисления

Исходный текст: [lambda.ref](https://github.com/Mazdaywik/Refal-05/blob/3.1/examples/lambda.ref)

Интерпретация:

        $ time echo 5 | refal lambda.ref
        Enter a number:
        120

        real    2m47,046s
        user    2m46,415s
        sys     0m0,014s

Скомпилировано Рефал-05 (`refal05c lambda.ref Library refal05rts` и переименовано)

        $ time echo 5 | ./lambda-05
        Enter a number:
        120

        real    3m32,260s
        user    3m30,360s
        sys     0m1,193s

Скомпилировано Рефал-5λ (`rlc lambda.ref -o lambda-5λ`)

        $ time echo 5 | ./lambda-5λ
        Enter a number:
        120

        real    3m31,053s
        user    3m30,366s
        sys     0m0,030s

#### Поиск простых чисел

Исходные тексты: [кириллический](examples/простые.реф) и [Базисный Рефал](examples/primes5.ref)

        $ time ./простые.реф > /dev/null

        real    0m0,944s
        user    0m0,943s
        sys     0m0,000s

        $ time ./primes5-05 > /dev/null

        real    0m4,646s
        user    0m1,381s
        sys     0m3,258s

        $ time ./primes5-5λ > /dev/null

        real    0m3,604s
        user    0m3,591s
        sys     0m0,004s

Компиляция в собственные опкоды при помощи Refal-5 Compiler. Version PZ Oct 29 2004
с последующим исполнением. 32-х разрядные исполняемые файлы, wine-6.9 (Staging).

        $ time ./REFC.EXE primes5.ref

        real    0m0,270s
        user    0m0,074s
        sys     0m0,056s

        $ time ./REFGO.EXE primes5.rsl  > /dev/null

        real    0m4,016s
        user    0m0,062s
        sys     0m0,074s
