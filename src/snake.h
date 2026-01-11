#ifndef SNAKE_H
#define SNAKE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#define PORT 22346
#define BUFFER_SIZE 8192

// max rozmery pre buffre/klienta (kvôli world[][] a mapbuf)
#define WORLD_WIDTH MAX_MAP_SIZE
#define WORLD_HEIGHT MAX_MAP_SIZE
 
// limity, ktoré bude hráč zadávať
#define MIN_MAP_SIZE 10
#define MAX_MAP_SIZE 25
 

#define INITIAL_SNAKE_LEN 3
#define FPS 5
#define MAX_OBSTACLES 7
#define MAX_CLIENTS 4
#define MAX_FRUITS 10

typedef enum {
    MSG_NEW_GAME = 1,
    MSG_MOVE = 2,
    MSG_GAME_STATE = 3,
    MSG_PAUSE = 4,
    MSG_RESUME = 5,
    MSG_QUIT = 6,
    MSG_JOIN_GAME = 7,
    MSG_PLAYER_NAME = 8,
    MSG_GAME_OVER = 9,
    MSG_INIT = 10
} MessageType;

typedef enum {
    UP = 0,
    DOWN = 1,
    LEFT = 2,
    RIGHT = 3,
    NONE = 4
} Direction;

typedef enum {
    MODE_STANDARD = 1,
    MODE_TIMED = 2
} GameMode;

typedef enum {
    WORLD_NO_OBSTACLES = 1,
    WORLD_WITH_OBSTACLES = 2
} WorldType;

typedef struct {
    MessageType type;
    int player_id;
    Direction direction;
    int game_id;
    int timestamp;
    char data[256];
} Message;

typedef struct {
    int id;
    int score;
    int alive;
    char name[50];
    Direction direction;
    Direction next_direction;
    int head_x;
    int head_y;
    int body_len;
    int body_x[1000];
    int body_y[1000];
} Player;

typedef struct {
    int id;
    int width;
    int height;
    int num_players;
    Player players[10];
    int fruit_x;
    int fruit_y;
    int num_fruits;
    int fruits[MAX_FRUITS][2];
    int obstacles[MAX_OBSTACLES][2];
    int num_obstacles;
    GameMode mode;
    WorldType world_type;
    int time_limit;
    int elapsed_time;
    int active;
    int game_over;
    time_t start_time;
} GameState;

#endif
