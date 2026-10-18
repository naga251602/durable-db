## Durable DB
A persistant key value store written in `c++20`

### Usage
cmd:
```
./durable-db <method> <options>
```

---
### Methods
| Method | function | 
| -------- | -------- |
| get(key)   | get the item from DB  |
| put(key, value)   | add the item to the DB  |
| drop(key)  | drop the item in the DB  |