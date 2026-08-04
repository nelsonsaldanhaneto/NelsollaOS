#ifndef GAME_SERVICE_H
#define GAME_SERVICE_H

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

// NELSOLLA RUN — endless runner escondido (easter egg). O Happy Mac do boot
// corre e pula bombas (o icone de erro do Mac classico). Um botao só:
// toque = pular, segurar = sair. Ativado por 5 toques rapidos em qualquer tela.
class GameService {
public:
    void begin(Adafruit_SSD1306* d, int touchPin);
    void start();
    void update();
    bool isActive() const { return active; }

private:
    enum Phase { PHASE_START, PHASE_RUNNING, PHASE_GAMEOVER };

    static const int TICK_MS = 33;          // ~30 fps
    static const int GROUND_Y = 58;
    static const int PLAYER_X = 12;
    static const int PLAYER_W = 12;
    static const int PLAYER_H = 14;
    static const int MAX_BOMBS = 3;
    static const unsigned long EXIT_HOLD_MS = 900;

    Adafruit_SSD1306* display = nullptr;
    int pin = -1;

    bool active = false;
    Phase phase = PHASE_START;

    unsigned long lastTick = 0;
    bool prevTouch = false;
    unsigned long touchStart = 0;
    bool exitConsumedTouch = false;

    float playerY = 0;      // topo do sprite
    float velocity = 0;
    bool onGround = true;

    float bombX[MAX_BOMBS];
    bool bombAlive[MAX_BOMBS];
    bool bombScored[MAX_BOMBS];
    float speed = 2.2f;

    int score = 0;
    int hiscore = 0;
    int groundPhase = 0;

    void resetRun();
    void spawnBomb(int slot, float x);
    void tickRunning(bool tapped);
    void render();
    void drawPlayer(int x, int y);
    void drawBomb(int cx);
    void drawCenteredText(const String& text, int y);
    void loadHiscore();
    void saveHiscore();
};

#endif
