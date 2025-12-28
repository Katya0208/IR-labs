# Information Retrieval Labs (Wikipedia: Applied_mathematics)

Проект для лабораторных по дисциплине «Информационный поиск».  
Корпус: статьи англоязычной Wikipedia по прикладной математике (категория `Category:Applied_mathematics), 30 000 документов.

---

## Структура проекта

- `robot.py` — загрузка корпуса из Wikipedia API (с возобновлением).
- `tokenize.cpp` — токенизация документов.
- `stemming.cpp` — стемминг токенов.
- `indexer.cpp` — построение булевого инвертированного индекса.
- `search_cli.cpp` — булев поиск по индексу (AND/OR/NOT, скобки).

---

## Требования

- Linux/WSL
- `g++` с поддержкой C++17
- `python3` (для загрузчика и построения графиков)
- `pip3 install matplotlib` для графиков

---

## 0) Сборка (из корня проекта)

```bash
g++ -O2 -std=c++17 tokenize.cpp -o tokenize
g++ -O2 -std=c++17 stemming.cpp -o stemming
g++ -O2 -std=c++17 -DSTEMMER_LIB zipf.cpp stemming.cpp -o zipf
g++ -O2 -std=c++17 -DSTEMMER_LIB indexer.cpp stemming.cpp -o indexer
g++ -O2 -std=c++17 -DSTEMMER_LIB search_cli.cpp stemming.cpp -o search_cli
```

## 1) Сбор корпуса (если корпуса ещё нет)

```bash
python3 robot.py
# результат: corpus/*.txt и manifest.jsonl
```

Проверка количества документов:
```bash
ls corpus/*.txt | wc -l
```

## 2) Токенизация → стемминг

```bash
./tokenize --dir ./corpus --report-mb 50
./stemming --dir ./corpus --report-mb 50
```

## 3) Построение индекса

```bash
mkdir -p out
./indexer --manifest ./manifest.jsonl --corpus ./corpus --out ./out --mem-mb 512 --report-mb 200
```

## 4) Запуск булевого поиска

```bash
echo 'function' | ./search_cli --index ./out --limit 5
echo '(algorithm || proof) !trivial' | ./search_cli --index ./out --limit 5
echo '(!network) (algorithm || proof)' | ./search_cli --index ./out --limit 5

```
