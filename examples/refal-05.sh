#!/bin/bash

# Пример исполнения транслятора Refal-05

echo 'Получаем исходный текст'

git clone https://github.com/Mazdaywik/Refal-05.git --branch 3.1 --depth 1

cp -r Refal-05/src ./

patch -p1 <refal-05.v3.1.patch

echo
echo 'Исполняем.'
echo 'Для корректной сборки Refal-05 самим собой должны быть заданы соотвествующие переменные окружения.'
echo

../refal src/refal05c.ref \
    Refal-05/src/refal05c \
    Refal-05/src/R05-AST  \
    Refal-05/src/R05-CompilerUtils \
    Refal-05/src/R05-Lexer \
    Refal-05/src/R05-Parser \
    Refal-05/src/R05-Generator \
    Refal-05/src/LibraryEx \
    Library refal05rts
