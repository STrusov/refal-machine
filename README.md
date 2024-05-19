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
(на данный момент поддерживается [Базисный РЕФАЛ](doc/%D0%A0%D0%95%D0%A4%D0%90%D0%9B-5%20%D0%A0%D1%83%D0%BA%D0%BE%D0%B2%D0%BE%D0%B4%D1%81%D1%82%D0%B2%D0%BE%20%D0%BF%D0%BE%20%D0%BF%D1%80%D0%BE%D0%B3%D1%80%D0%B0%D0%BC%D0%BC%D0%B8%D1%80%D0%BE%D0%B2%D0%B0%D0%BD%D0%B8%D1%8E%20%D0%B8%20%D1%81%D0%BF%D1%80%D0%B0%D0%B2%D0%BE%D1%87%D0%BD%D0%B8%D0%BA%20%D0%A2%D1%83%D1%80%D1%87%D0%B8%D0%BD%20%D0%92.%D0%A4.pdf), за исключением нескольких встроенных функций) в ОС Linux. Реализована на яыке Си и имеет минимум зависимостей (в том числе от стандартной библиотеки Си). Представляет собой (потенциально) встраиваемый интерпретатор, предварительно транслирующий исходный текст в байт-код, который и исполняется. Трансляция выполняется в один проход, однако, поддерживается традиционный для РЕФАЛ произвольный порядок определения функций.

Основные отличия:
* Поддержка кириллицы (и практически любых Уникод-сомволов) в идентификаторах.
* Префиксы `s.` `t.` и `e.` переменных могут быть соответственно `?` `!` и `.` или `…`
* Начинающиеся с `$` директивы упразднены (добавлена [поддержка модулей](#поддержка-модулей)).
* В вычислительных скобках `< >` имя вызываемой функции не обязательно должно
идти непосредственно после открывающей скобки, перед ним можно расположить данные
или простой идентификатор. Транслятор знает, что можно вызвать. Это же послабление
относится и к функции `Mu`, которая выполняет поиск вызываемой функции среди
аргументов во время выполнения (пропуская структурные скобки).
* Имя вызываемой функции можно указывать `Mu` в виде текстовой строки, в том числе
включающего предшествующее имя модуля, отделённое пробелом. При этом применимо
вышеуказанное правило: `Mu` рассмотрит строку целиком (до первого отличного)
от буквы аргумента; если функция с таким именем существует и вычислима, она будет
вызвана, иначе принимается за аргумент вызываемой функции.
* Имена функций, не содержащих вычисляемых предложений (подобие [ENUM Рефал-05](https://github.com/Mazdaywik/Refal-05?tab=readme-ov-file#особенности-языка)),
можно использовать для «копилки». Произвольные имена «ящиков» не допускаются.
Функция `Push`, в отличие от `Br` Рефал-5, не требует символ `=` для разделения
имени «ящика» и данных. Идентификатор ящика определяется подобно `Mu` и
может располагаться среди прочего. Вместо `Dg` используется `Pop`.
* Помимо точки входа `Go` возможно использовать `Main`, которая принимает в поле
зрения аргументы командной строки (каждый заключён в структурные скобки, как в
результате `ArgList` из [LibraryEx](https://github.com/Mazdaywik/refal-5-framework/blob/master/docs/LibraryEx.md#функция-arglist)).
Кроме того, поддерживается точка входа `Начало` — в таком случае интерпретатор
проверяет, содержит ли первое её предложение образец, надо ли передавать аргументы.
Допустимы вышеперечисленные имена, начинающиеся со строчной буквы — результат их
исполнения выводится, а не отбрасывается.
* Поддержаны функции из одного предложения, без блока `{ }`.

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

Компиляция в байт-код при помощи Refal-5 Compiler. Version PZ Oct 29 2004
с последующим исполнением. 32-х разрядные исполняемые файлы, wine-6.9 (Staging).

        $ time ./REFC.EXE primes5.ref

        real    0m0,270s
        user    0m0,074s
        sys     0m0,056s

        $ time ./REFGO.EXE primes5.rsl  > /dev/null

        real    0m4,016s
        user    0m0,062s
        sys     0m0,074s
