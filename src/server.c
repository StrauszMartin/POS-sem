#include "snake.h"

#define MAX_CLIENTS 10

typedef struct {
    int socket;
    int player_id;
    char name[50];
} Client;

GameState game;
Client clients[MAX_CLIENTS];
int num_clients = 0;
pthread_mutex_t game_mutex = PTHREAD_MUTEX_INITIALIZER;
int running = 1;

void generate_obstacles_random(int count) {
    game.num_obstacles = 0;
    int attempts = 0;

    while (game.num_obstacles < count && attempts < count * 3) {
        int x = rand() % game.width;
        int y = rand() % game.height;

        int valid = 1;
        for (int i = 0; i < game.num_players; i++) {
            if (game.players[i].head_x == x && game.players[i].head_y == y) {
                valid = 0;
                break;
            }
        }

        if (valid && game.obstacles[y][x] != 1) {
            game.obstacles[y][x] = 1;
            game.num_obstacles++;
        }

        attempts++;
    }

    fprintf(stderr, "[SERVER] %d prekážok vygenerovaných\n", game.num_obstacles);
}

void init_game(int width, int height, GameMode mode, int time_limit, WorldType world_type) {
    game.id = rand() % 10000;
    game.width = width;
    game.height = height;
    game.num_players = 0;
    game.mode = mode;
    game.time_limit = time_limit;
    game.elapsed_time = 0;
    game.active = 1;
    game.game_over = 0;
    game.world_type = world_type;
    game.start_time = time(NULL);

    for (int y = 0; y < game.height; y++) {
        for (int x = 0; x < game.width; x++) {
            game.obstacles[y][x] = 0;
        }
    }

    if (world_type == WORLD_WITH_OBSTACLES) {
        int obstacle_count = (width * height) / 8;
        generate_obstacles_random(obstacle_count);
    }

    int valid = 0;
    while (!valid) {
        game.fruit_x = rand() % width;
        game.fruit_y = rand() % height;
        valid = 1;
        if (game.obstacles[game.fruit_y][game.fruit_x] == 1) {
            valid = 0;
        }
    }

    fprintf(stderr, "[SERVER] Hra inicializovaná: %dx%d, režim: %d, svet: %d\n",
            width, height, mode, world_type);
}

void init_snake(int player_id, const char* name) {
    if (game.num_players >= 10) return;

    Player* p = &game.players[game.num_players];
    p->id = player_id;
    p->alive = 1;
    p->score = 0;
    strcpy(p->name, name);
    p->direction = RIGHT;
    p->next_direction = RIGHT;
    p->head_x = 5 + game.num_players * 5;
    p->head_y = game.height / 2;
    p->body_len = INITIAL_SNAKE_LEN;

    for (int i = 0; i < INITIAL_SNAKE_LEN; i++) {
        p->body_x[i] = p->head_x - i;
        p->body_y[i] = p->head_y;
    }

    game.num_players++;
    fprintf(stderr, "[SERVER] Hadík '%s' vytvorený (ID: %d)\n", name, player_id);
}

void spawn_fruit() {
    int valid = 0;
    while (!valid) {
        game.fruit_x = rand() % game.width;
        game.fruit_y = rand() % game.height;
        valid = 1;

        if (game.obstacles[game.fruit_y][game.fruit_x] == 1) {
            valid = 0;
            continue;
        }

        for (int i = 0; i < game.num_players; i++) {
            if (!game.players[i].alive) continue;
            for (int j = 0; j < game.players[i].body_len; j++) {
                if (game.players[i].body_x[j] == game.fruit_x &&
                    game.players[i].body_y[j] == game.fruit_y) {
                    valid = 0;
                    break;
                }
            }
        }
    }
    fprintf(stderr, "[SERVER] Ovocie vygenerované: (%d, %d)\n", game.fruit_x, game.fruit_y);
}

void update_snake(Player* p) {
    if (!p->alive) return;

    p->direction = p->next_direction;

    int new_x = p->head_x;
    int new_y = p->head_y;

    switch (p->direction) {
        case UP: new_y--; break;
        case DOWN: new_y++; break;
        case LEFT: new_x--; break;
        case RIGHT: new_x++; break;
        case NONE: break;
    }

    if (game.world_type == WORLD_NO_OBSTACLES) {
        if (new_x < 0) new_x = game.width - 1;
        if (new_x >= game.width) new_x = 0;
        if (new_y < 0) new_y = game.height - 1;
        if (new_y >= game.height) new_y = 0;
    } else {
        if (new_x < 0 || new_x >= game.width || new_y < 0 || new_y >= game.height) {
            p->alive = 0;
            fprintf(stderr, "[SERVER] Hadík '%s' narazil do okraja!\n", p->name);
            return;
        }
    }

    if (game.obstacles[new_y][new_x] == 1) {
        p->alive = 0;
        fprintf(stderr, "[SERVER] Hadík '%s' narazil do prekážky!\n", p->name);
        return;
    }

    for (int i = 0; i < p->body_len - 1; i++) {
        if (p->body_x[i] == new_x && p->body_y[i] == new_y) {
            p->alive = 0;
            fprintf(stderr, "[SERVER] Hadík '%s' narazil sám do seba!\n", p->name);
            return;
        }
    }

    for (int i = 0; i < game.num_players; i++) {
        if (i == p->id || !game.players[i].alive) continue;
        for (int j = 0; j < game.players[i].body_len; j++) {
            if (game.players[i].body_x[j] == new_x && game.players[i].body_y[j] == new_y) {
                p->alive = 0;
                fprintf(stderr, "[SERVER] Hadík '%s' narazil do iného hadíka!\n", p->name);
                return;
            }
        }
    }

    for (int i = p->body_len - 1; i > 0; i--) {
        p->body_x[i] = p->body_x[i - 1];
        p->body_y[i] = p->body_y[i - 1];
    }

    p->head_x = new_x;
    p->head_y = new_y;
    p->body_x[0] = new_x;
    p->body_y[0] = new_y;

    if (p->head_x == game.fruit_x && p->head_y == game.fruit_y) {
        p->score += 10;
        p->body_len++;
        fprintf(stderr, "[SERVER] Hadík '%s' zjedol ovocie! Body: %d\n", p->name, p->score);
        spawn_fruit();
    }
}

void* game_loop(void* arg) {
    while (running) {
        pthread_mutex_lock(&game_mutex);

        if (game.active) {
            game.elapsed_time = (int)(time(NULL) - game.start_time);

            if (game.mode == MODE_TIMED && game.elapsed_time >= game.time_limit) {
                game.active = 0;
                game.game_over = 1;
                fprintf(stderr, "[SERVER] Čas vypršal! KONIEC HRY!\n");
            } else {
                for (int i = 0; i < game.num_players; i++) {
                    if (game.players[i].alive) {
                        update_snake(&game.players[i]);
                    }
                }

                int alive_count = 0;
                for (int i = 0; i < game.num_players; i++) {
                    if (game.players[i].alive) alive_count++;
                }

                if (alive_count == 0) {
                    game.active = 0;
                    game.game_over = 1;
                    fprintf(stderr, "[SERVER] Žiaden živý hadík! KONIEC HRY!\n");
                }
            }
        }

        pthread_mutex_unlock(&game_mutex);
        usleep(1000000 / FPS);
    }
    return NULL;
}

void send_game_state(int client_socket) {
    pthread_mutex_lock(&game_mutex);

    char response[BUFFER_SIZE];
    snprintf(response, BUFFER_SIZE,
             "STATE|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|",
             game.id, game.width, game.height, game.num_players,
             game.fruit_x, game.fruit_y, game.active, game.game_over,
             game.num_obstacles, game.mode, game.world_type, game.elapsed_time);

    for (int y = 0; y < game.height; y++) {
        for (int x = 0; x < game.width; x++) {
            if (game.obstacles[y][x] == 1) {
                char obs_str[30];
                snprintf(obs_str, 30, "O|%d|%d|", x, y);
                if (strlen(response) + strlen(obs_str) + 1 < BUFFER_SIZE)
                    strcat(response, obs_str);
            }
        }
    }

    for (int i = 0; i < game.num_players; i++) {
        Player* p = &game.players[i];
        char player_str[200];
        snprintf(player_str, 200,
                 "P|%d|%s|%d|%d|%d|%d|%d|%d|",
                 p->id, p->name, p->alive, p->score,
                 p->head_x, p->head_y, p->body_len, p->direction);
        if (strlen(response) + strlen(player_str) + 1 < BUFFER_SIZE)
            strcat(response, player_str);
    }

    send(client_socket, response, strlen(response), 0);
    pthread_mutex_unlock(&game_mutex);
}

void handle_client_message(const char* buffer) {
    if (strncmp(buffer, "NEW_GAME", 8) == 0) {
        int mode, world_type, time_limit;
        sscanf(buffer, "NEW_GAME|%d|%d|%d", &mode, &world_type, &time_limit);
        init_game(WORLD_WIDTH, WORLD_HEIGHT, (GameMode)mode, time_limit, (WorldType)world_type);
        fprintf(stderr, "[SERVER] Nová hra: režim=%d, svet=%d, čas=%d\n",
                mode, world_type, time_limit);
    } else if (strncmp(buffer, "PLAYER", 6) == 0) {
        char name[50];
        sscanf(buffer, "PLAYER|%49[^|]", name);
        init_snake(game.num_players, name);
    } else if (strncmp(buffer, "MOVE", 4) == 0) {
        int pid, dir;
        sscanf(buffer, "MOVE|%d|%d", &pid, &dir);
        if (pid < game.num_players && game.players[pid].alive)
            game.players[pid].next_direction = (Direction)dir;
    } else if (strncmp(buffer, "QUIT", 4) == 0) {
        int pid;
        sscanf(buffer, "QUIT|%d", &pid);
        if (pid < game.num_players) {
            game.players[pid].alive = 0;
            fprintf(stderr, "[SERVER] Hráč %d opustil hru\n", pid);
        }
    }
}

void* client_handler(void* arg) {
    int client_socket = *(int*)arg;
    free(arg);

    char buffer[BUFFER_SIZE];

    while (running) {
        int n = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (n <= 0) break;

        buffer[n] = '\0';

        pthread_mutex_lock(&game_mutex);
        handle_client_message(buffer);
        pthread_mutex_unlock(&game_mutex);

        send_game_state(client_socket);
    }

    close(client_socket);
    fprintf(stderr, "[SERVER] Klient odpojený\n");
    return NULL;
}

int main() {
    fprintf(stderr, "SERVER HADIK - port %d\n", PORT);

    srand(time(NULL));

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "[SERVER] Čaká sa na klientov...\n");

    pthread_t game_thread;
    pthread_create(&game_thread, NULL, game_loop, NULL);

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) continue;

        fprintf(stderr, "[SERVER] Klient sa pripojil: %s:%d\n",
                inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        int* sock = malloc(sizeof(int));
        *sock = client_socket;

        pthread_t thread;
        pthread_create(&thread, NULL, client_handler, sock);
        pthread_detach(thread);
    }

    running = 0;
    pthread_join(game_thread, NULL);
    close(server_sock);

    fprintf(stderr, "[SERVER] Server sa vypína...\n");
    return 0;
}
