#!/bin/bash

# Пример исполнения examples/lambda.ref из Refal-05

echo 'Получаем исходный текст'

git clone https://github.com/Mazdaywik/Refal-05.git --branch 3.1 --depth 1

cp Refal-05/examples/lambda.ref ./

patch -p2 <lambda.patch

echo
echo 'Исполняем'
echo

../refal lambda.ref
