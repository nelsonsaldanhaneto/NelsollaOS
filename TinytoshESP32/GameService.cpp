#include "GameService.h"
#include <Preferences.h>

// Namespace proprio no NVS: nao mistura com a config do ConfigManager.
static const char* GAME_PREFS = "nellyrun";

void GameService::begin(Adafruit_SSD1306* d, int touchPin) {
    display = d;
    pin = touchPin;
    loadHiscore();
}

void GameService::loadHiscore() {
    Preferences prefs;
    prefs.begin(GAME_PREFS, true);
    hiscore = prefs.getInt("hiscore", 0);
    prefs.end();
}

void GameService::saveHiscore() {
    Preferences prefs;
    prefs.begin(GAME_PREFS, false);
    prefs.putInt("hiscore", hiscore);
    prefs.end();
}

void GameService::start() {
    active = true;
    phase = PHASE_START;
    // O gesto de ativacao (clique + segurar 3s) termina com o botao AINDA
    // pressionado. Marcar esse toque como ja consumido evita que ele seja lido
    // como "segurar para sair" e feche o jogo no mesmo instante em que abre.
    prevTouch = true;
    exitConsumedTouch = true;
    touchStart = millis();
    lastTick = 0;
    Serial.println("GameService: NELSOLLA RUN ativado!");
    render();
}

void GameService::resetRun() {
    playerY = GROUND_Y - PLAYER_H;
    velocity = 0;
    onGround = true;
    speed = 2.2f;
    score = 0;
    groundPhase = 0;
    for (int i = 0; i < MAX_BOMBS; i++) { bombAlive[i] = false; bombScored[i] = false; }
    spawnBomb(0, 140);
}

void GameService::spawnBomb(int slot, float x) {
    bombX[slot] = x;
    bombAlive[slot] = true;
    bombScored[slot] = false;
}

void GameService::update() {
    if (!active || display == nullptr) return;

    unsigned long now = millis();
    if (now - lastTick < TICK_MS) return;
    lastTick = now;

    // Entrada: borda de subida = toque; segurar EXIT_HOLD_MS = sair.
    bool touch = (digitalRead(pin) == HIGH);
    bool tapped = touch && !prevTouch;
    if (tapped) { touchStart = now; exitConsumedTouch = false; }

    if (touch && !exitConsumedTouch && (now - touchStart >= EXIT_HOLD_MS)) {
        exitConsumedTouch = true;
        active = false;
        Serial.println("GameService: saindo do jogo.");
        prevTouch = touch;
        return;
    }
    prevTouch = touch;

    switch (phase) {
        case PHASE_START:
            if (tapped) { resetRun(); phase = PHASE_RUNNING; }
            break;

        case PHASE_RUNNING:
            tickRunning(tapped);
            break;

        case PHASE_GAMEOVER:
            if (tapped) { resetRun(); phase = PHASE_RUNNING; }
            break;
    }

    render();
}

void GameService::tickRunning(bool tapped) {
    // Pulo
    if (tapped && onGround) {
        velocity = -3.6f;
        onGround = false;
    }
    if (!onGround) {
        velocity += 0.34f;
        playerY += velocity;
        if (playerY >= GROUND_Y - PLAYER_H) {
            playerY = GROUND_Y - PLAYER_H;
            velocity = 0;
            onGround = true;
        }
    }

    // Bombas andam; pontua ao passar; respawna com folga aleatoria
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (!bombAlive[i]) continue;
        bombX[i] -= speed;

        if (!bombScored[i] && bombX[i] + 8 < PLAYER_X) {
            bombScored[i] = true;
            score++;
            // Acelera aos poucos; teto pra continuar jogavel
            if (score % 4 == 0 && speed < 5.0f) speed += 0.25f;
            // Segunda bomba entra no jogo a partir de 6 pontos
            if (score == 6 && !bombAlive[1]) spawnBomb(1, 190);
        }

        if (bombX[i] < -10) {
            float maxX = 128;
            for (int j = 0; j < MAX_BOMBS; j++)
                if (bombAlive[j] && bombX[j] > maxX) maxX = bombX[j];
            float gap = 70 + (esp_random() % 60);   // 70..129 px
            spawnBomb(i, maxX + gap);
        }
    }

    // Colisao AABB (bomba: circulo r=4 no chao, caixa 8x8)
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (!bombAlive[i]) continue;
        int bx = (int)bombX[i] - 4, by = GROUND_Y - 8;
        bool hitX = (PLAYER_X + PLAYER_W - 2 > bx) && (PLAYER_X + 2 < bx + 8);
        bool hitY = (playerY + PLAYER_H > by);
        if (hitX && hitY) {
            phase = PHASE_GAMEOVER;
            if (score > hiscore) { hiscore = score; saveHiscore(); }
            return;
        }
    }

    groundPhase = (groundPhase + (int)speed) % 16;
}

void GameService::drawPlayer(int x, int y) {
    // Mini Happy Mac: gabinete + tela acesa
    display->drawRect(x, y, PLAYER_W, PLAYER_H, 1);
    display->fillRect(x + 2, y + 2, PLAYER_W - 4, 6, 1);
    display->drawFastHLine(x + 2, y + PLAYER_H - 3, 4, 1);
}

void GameService::drawBomb(int cx) {
    display->fillCircle(cx, GROUND_Y - 5, 4, 1);
    display->drawLine(cx + 2, GROUND_Y - 9, cx + 5, GROUND_Y - 12, 1);
    display->drawPixel(cx + 6, GROUND_Y - 13, 1);
}

void GameService::drawCenteredText(const String& text, int y) {
    int16_t x1, y1; uint16_t w, h;
    display->getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
    display->setCursor((128 - w) / 2, y);
    display->print(text);
}

void GameService::render() {
    display->clearDisplay();
    display->setTextColor(SSD1306_WHITE);
    display->setTextWrap(false);
    display->setFont();
    display->setTextSize(1);

    // Faixa amarela: placar sempre visivel
    display->setCursor(4, 3);
    display->print("PTS " + String(score));
    String rec = "REC " + String(hiscore);
    int16_t x1, y1; uint16_t w, h;
    display->getTextBounds(rec.c_str(), 0, 0, &x1, &y1, &w, &h);
    display->setCursor(124 - w, 3);
    display->print(rec);
    display->drawFastHLine(0, 13, 128, 1);

    if (phase == PHASE_START) {
        drawPlayer(16, 30);
        drawCenteredText("NELSOLLA RUN", 24);
        drawCenteredText("Toque: pular", 38);
        drawCenteredText("Segure: sair", 48);
    } else {
        // Chao com tracos andando (sensacao de velocidade)
        display->drawFastHLine(0, GROUND_Y, 128, 1);
        for (int gx = -groundPhase; gx < 128; gx += 16)
            if (gx >= 0) display->drawFastHLine(gx, GROUND_Y + 3, 6, 1);

        drawPlayer(PLAYER_X, (int)playerY);
        for (int i = 0; i < MAX_BOMBS; i++)
            if (bombAlive[i] && bombX[i] > -10 && bombX[i] < 136) drawBomb((int)bombX[i]);

        if (phase == PHASE_GAMEOVER) {
            display->fillRect(14, 22, 100, 26, 0);
            display->drawRect(14, 22, 100, 26, 1);
            drawCenteredText("GAME OVER", 26);
            drawCenteredText(score >= hiscore && score > 0 ? "NOVO RECORDE!" : "Toque: de novo", 38);
        }
    }

    display->display();
}
