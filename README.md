# GTKGram - клиент Telegram на GTK4 и C. 

## Что есть

1. Просмотр сообщений
2. Список чатов
3. Вход по QR
4. Отправка сообщений
5. Отображение изображений
6. Стикеры (частично)

## Сборка

```
nix build . # для NixOS
```

## Запуск

Для запуска потребуется получить свой TG_API и TG_API_HASH на [my.telegram.org](https://my.telegram.org).

После получения:

```
export TG_API=<значение>
export TG_API_HASH=<значение>
./result/bin/tgtk
```

