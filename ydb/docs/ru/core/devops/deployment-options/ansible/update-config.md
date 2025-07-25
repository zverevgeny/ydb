# Обновление конфигурации кластеров {{ ydb-short-name }}, развёрнутых с Ansible

Во время [первоначального развёртывания](initial-deployment.md) Ansible playbook использует предоставленный конфигурационный файл для создания начальной конфигурации кластера. Технически, он генерирует два варианта конфигурационного файла на основе исходного и размещает их на всех хостах через механизм Ansible для копирования файлов между серверами. В этой статье рассматриваются доступные способы для изменения конфигурации кластера после первоначального развёртывания.

## Обновление конфигурации через Ansible playbook

В репозитории [ydb-ansible](https://github.com/ydb-platform/ydb-ansible) есть playbook под названием `ydb_platform.ydb.update_config`, который можно использовать для обновления конфигурации кластера {{ ydb-short-name }}. Перейдите в ту же директорию, которая использовалась для [первоначального развёртывания](initial-deployment.md) кластера, отредактируйте файл `files/config.yaml` по необходимости и затем запустите этот playbook:

```bash
ansible-playbook ydb_platform.ydb.update_config
```

Этот playbook развернёт новую версию конфигурационных файлов и затем выполнит [постепенную перезагрузку](restart.md) кластера.

### Фильтрация по типу узла

Задачи в playbook `ydb_platform.ydb.update_config` помечены типами узлов, поэтому вы можете использовать функциональность тегов Ansible для фильтрации узлов по их типу.

Эти две команды эквивалентны и изменят конфигурацию всех [узлов хранения](../../../concepts/glossary.md#storage-node):

```bash
ansible-playbook ydb_platform.ydb.update_config --tags storage
ansible-playbook ydb_platform.ydb.update_config --tags static
```

Эти две команды эквивалентны и изменят конфигурацию всех [узлов баз данных](../../../concepts/glossary.md#database-node):

```bash
ansible-playbook ydb_platform.ydb.update_config --tags database
ansible-playbook ydb_platform.ydb.update_config --tags dynamic
```

### Пропуск перезагрузки

Также есть тег `no_restart`, который позволяет только обновить конфигурационные файлы и пропустить перезагрузку кластера. Это может быть полезно, если кластер будет [перезагружен](restart.md) позже вручную или в рамках других задач по обслуживанию. Пример запуска:

```bash
ansible-playbook ydb_platform.ydb.update_config --tags no_restart
```
