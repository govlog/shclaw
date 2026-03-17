# API Plugin

Les plugins sont des fichiers `.c` unitaires que les agents écrivent à la volée. Ils sont compilés en mémoire par TCC -- aucun `.so` n'est jamais écrit sur le disque.

## Structure

Chaque plugin inclut `tc_plugin.h` et exporte quatre choses :

```c
#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "weather";
const char *TC_PLUGIN_DESC = "Récupérer la météo pour une ville";
const char *TC_PLUGIN_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\",\"description\":\"Nom de la ville\"}},\"required\":[\"city\"]}";

const char *tc_execute(const char *input_json) {
    void *json = tc_json_parse(input_json);
    const char *city = tc_json_string(tc_json_get(json, "city"));

    char url[256];
    tc_snprintf(url, sizeof(url), "https://wttr.in/%s?format=3", city);

    static char result[512];
    int status = tc_http_get(url, result, sizeof(result));
    tc_json_free(json);

    return (status == 200) ? result : "erreur";
}
```

Le template builder est dans [`plugins/_template.c`](../plugins/_template.c). Les fichiers commençant par `_` sont ignorés par le scanner.

## Sandbox

Les plugins tournent en mode `-nostdlib` : pas de libc, pas de headers système. Le daemon injecte un ensemble de fonctions `tc_*` via `tcc_add_symbol()` avant la compilation. Les plugins ont HTTP+TLS, JSON et I/O fichier gratuitement.

## Fonctions disponibles

| Catégorie | Fonctions |
|-----------|-----------|
| Mémoire | `tc_malloc`, `tc_free` |
| Chaînes | `tc_strlen`, `tc_strcmp`, `tc_strncmp`, `tc_strcpy`, `tc_strncpy`, `tc_snprintf`, `tc_memcpy`, `tc_memset`, `tc_strstr`, `tc_strchr`, `tc_atoi` |
| Fichiers | `tc_read_file`, `tc_write_file` |
| HTTP | `tc_http_get`, `tc_http_post`, `tc_http_post_json`, `tc_http_header` |
| JSON | `tc_json_parse`, `tc_json_free`, `tc_json_print`, `tc_json_get`, `tc_json_index`, `tc_json_array_size`, `tc_json_string`, `tc_json_int`, `tc_json_double` |
| Système | `tc_gethostname` |
| Logging | `tc_log` |

Les appels HTTP passent par la stack BearSSL du daemon.

Voir [`include/tc_plugin.h`](../include/tc_plugin.h) pour les déclarations complètes.
