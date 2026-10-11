// agujero_negro_v3.cpp
//
// v3: sobre la base del pipeline anterior (RenderTexture2D + Fragment Shader
// de post-proceso, ya validado a 60 FPS), se agrega:
//   - Efecto Doppler relativista + beaming por particula (calculado en C++,
//     no en el shader, porque ahi tenemos el vector de velocidad real).
//   - Caida no lineal mas pronunciada en el shader de lente (mas parecida
//     a la fotosfera de un agujero negro de Schwarzschild).
//   - Disco de acrecion ampliado a 1500 particulas.
//   - FIX: DrawSphere() solo recibe UN radio. La firma real es
//     DrawSphere(Vector3 centerPos, float radius, Color color) -- antes
//     se llamaba con el radio duplicado y no compilaba.
//
// Fisica de particulas, embudo gravitatorio y camara orbital: igual que
// en la version anterior.

#include "raylib.h"
#include <vector>
#include <cmath>


struct Particula3D {
    Vector3 pos;
    Vector3 vel;
    Color color;
    float tamano;
};

// Satura un float a un byte de color valido (0-255)
static inline unsigned char SaturarByte(float v) {
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (unsigned char)v;
}

// Doppler relativista + beaming para una particula, segun su velocidad
// relativa a la camara. Se calcula por particula en C++ (no en el shader
// 2D de post-proceso) porque aqui tenemos el vector de velocidad 3D real;
// el shader de lente solo ve la imagen ya compuesta en pixeles.
//
//   cosTheta   : angulo entre la velocidad de la particula y la direccion
//                hacia la camara (+1 se acerca de frente, -1 se aleja)
//   beta       : v/c "visual" (escalado con dopplerEscala para que quede
//                en un rango razonable con las velocidades de la sim)
//   gamma      : factor de Lorentz, 1 / sqrt(1 - beta^2)
//   D          : factor Doppler relativista, 1 / (gamma * (1 - beta*cosTheta))
//   brillo     : beaming relativista, aproximado como D^3
Color AplicarDopplerBeaming(Vector3 pos, Vector3 vel, Color base, Vector3 camPos, float dopplerEscala) {
    float vx = camPos.x - pos.x;
    float vy = camPos.y - pos.y;
    float vz = camPos.z - pos.z;
    float viewLen = sqrtf(vx * vx + vy * vy + vz * vz);
    if (viewLen < 0.0001f) viewLen = 0.0001f;
    vx /= viewLen; vy /= viewLen; vz /= viewLen;

    float speed = sqrtf(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    float cosTheta = 0.0f;
    if (speed > 0.0001f) {
        cosTheta = (vel.x * vx + vel.y * vy + vel.z * vz) / speed;
    }

    float beta = speed / dopplerEscala;
    if (beta > 0.95f) beta = 0.95f;
    float gamma = 1.0f / sqrtf(1.0f - beta * beta);
    float dopplerFactor = 1.0f / (gamma * (1.0f - beta * cosTheta));

    float brillo = powf(dopplerFactor, 3.0f);
    if (brillo > 3.5f) brillo = 3.5f;
    if (brillo < 0.15f) brillo = 0.15f;

    float r = (float)base.r;
    float g = (float)base.g;
    float b = (float)base.b;

    if (dopplerFactor >= 1.0f) {
        // Se acerca: vira a blanco/azul brillante (blueshift)
        float t = fminf(dopplerFactor - 1.0f, 1.2f) / 1.2f;
        r += t * (255.0f - r);
        g += t * (255.0f - g);
        b = 255.0f;
    } else {
        // Se aleja: se apaga y enrojece (redshift)
        float t = fminf(1.0f - dopplerFactor, 1.0f);
        g *= (1.0f - 0.85f * t);
        b *= (1.0f - 0.95f * t);
    }

    r *= brillo; g *= brillo; b *= brillo;

    Color out;
    out.r = SaturarByte(r);
    out.g = SaturarByte(g);
    out.b = SaturarByte(b);
    out.a = 255;
    return out;
}

int main() {
    const int ancho = 1280;
    const int alto = 720;
    InitWindow(ancho, alto, "Agujero Negro 3D v3 - Lente + Doppler Relativista");
    SetTargetFPS(60);

    // --- CAMARA 3D ---
    Camera3D camera = { 0 };
    camera.target = (Vector3){ 0.0f, -0.5f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    float camAngleX = 0.5f;
    float camAngleY = 0.35f;
    float camDistancia = 28.0f;

    Vector3 centro = { 0.0f, 0.0f, 0.0f };
    float radioHorizonte = 2.2f;
    float masaBH = 130.0f;
    float radioMaximo = 18.0f;
    float dopplerEscala = 7.5f; // escala visual de v/c para el Doppler (ajustable)

    // --- DISCO DE ACRECION (1500 particulas) ---
    const int numParticulas = 1500;
    std::vector<Particula3D> particulas;
    particulas.reserve(numParticulas);
    for (int i = 0; i < numParticulas; i++) {
        float r = (float)GetRandomValue(45, 170) / 10.0f;
        float angulo = (float)GetRandomValue(0, 3600) * (3.14159265f / 1800.0f);

        float x = r * cosf(angulo);
        float z = r * sinf(angulo);
        float y = ((float)GetRandomValue(-2, 2) / 20.0f);

        float v = sqrtf(masaBH / r);
        Vector3 vel = { -sinf(angulo) * v, 0.0f, cosf(angulo) * v };

        Color col = ORANGE;
        float sz = 0.15f;
        if (r < 7.5f) { col = GOLD; sz = 0.18f; }
        if (r < 4.2f) { col = WHITE; sz = 0.20f; }

        particulas.push_back({ {x, y, z}, vel, col, sz });
    }

    // ================== LENTE GRAVITACIONAL (SHADER GLSL) ==================
    // Fragment shader embebido: se aplica como post-proceso sobre la escena
    // ya renderizada, curvando los pixeles alrededor de la posicion en
    // pantalla del agujero negro. No depende de ningun archivo .fs externo.
    const char *fsLente = R"GLSL(
        #version 330

        in vec2 fragTexCoord;
        in vec4 fragColor;

        uniform sampler2D texture0;
        uniform vec4 colDiffuse;

        uniform vec2 resolution;
        uniform vec2 bhScreenPos;
        uniform float horizonRadius;
        uniform float lensStrength;

        out vec4 finalColor;

        void main()
        {
            vec2 fragPixel = fragTexCoord * resolution;
            vec2 diff = fragPixel - bhScreenPos;
            float dist = length(diff);
// DrawSphereWires(centro, radioHorizonte + 0.01f, 16, 16, RED);
            // Dentro del horizonte de sucesos: negro absoluto
            if (dist < horizonRadius)
            {
                finalColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }

            vec2 dir = diff / max(dist, 0.0001);

            // Caida no lineal (cuadratica) en vez de lineal: concentra la
            // curvatura mucho mas cerca del horizonte -- una aproximacion
            // mas cercana a como se comporta la luz cerca de la fotosfera
            // de un agujero negro de Schwarzschild -- y cae mas rapido
            // lejos, en vez de un "smear" amplio y difuso.
            float u = horizonRadius / dist; // 0..1, =1 justo en el borde
            float falloff = u * u;
            float displacement = lensStrength * horizonRadius * falloff;

            vec2 samplePixel = fragPixel + dir * displacement;
            vec2 sampleUV = clamp(samplePixel / resolution, 0.0, 1.0);

            vec3 color = texture(texture0, sampleUV).rgb;

            // Anillo de luz brillante (photon ring) justo en el borde,
            // mas concentrado que antes para un look mas "Interstellar"
            float ring = 1.0 - smoothstep(horizonRadius, horizonRadius * 1.25, dist);
            color += vec3(1.0, 0.85, 0.55) * ring * 0.7;

            finalColor = vec4(color, 1.0);
        }
    )GLSL";

    Shader lenteShader = LoadShaderFromMemory(0, fsLente);
    int locResolution    = GetShaderLocation(lenteShader, "resolution");
    int locBhScreenPos   = GetShaderLocation(lenteShader, "bhScreenPos");
    int locHorizonRadius = GetShaderLocation(lenteShader, "horizonRadius");
    int locLensStrength  = GetShaderLocation(lenteShader, "lensStrength");

    float resolution[2] = { (float)ancho, (float)alto };
    SetShaderValue(lenteShader, locResolution, resolution, SHADER_UNIFORM_VEC2);

    float lensStrength = 1.8f; // intensidad visual de la curvatura (ajustable en vivo)

    RenderTexture2D sceneTarget = LoadRenderTexture(ancho, alto);
    SetTextureFilter(sceneTarget.texture, TEXTURE_FILTER_BILINEAR);
    // =========================================================================

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        // --- CONTROL DE CAMARA (CLIC DERECHO) ---
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 delta = GetMouseDelta();
            camAngleX -= delta.x * 0.005f;
            camAngleY += delta.y * 0.005f;

            if (camAngleY < 0.08f) camAngleY = 0.08f;
            if (camAngleY > 1.40f) camAngleY = 1.40f;
        }

        camera.position.x = centro.x + camDistancia * cosf(camAngleY) * sinf(camAngleX);
        camera.position.y = centro.y + camDistancia * sinf(camAngleY);
        camera.position.z = centro.z + camDistancia * cosf(camAngleY) * cosf(camAngleX);

        // --- AJUSTE EN VIVO DE LA INTENSIDAD DE LA LENTE (Flechas arriba/abajo) ---
        if (IsKeyDown(KEY_UP))   lensStrength += 1.0f * dt;
        if (IsKeyDown(KEY_DOWN)) lensStrength -= 1.0f * dt;
        if (lensStrength < 0.0f) lensStrength = 0.0f;
        SetShaderValue(lenteShader, locLensStrength, &lensStrength, SHADER_UNIFORM_FLOAT);

        // --- FISICA FLUIDA DE PARTICULAS (sin cambios) ---
        for (auto& p : particulas) {
            float rx = centro.x - p.pos.x;
            float rz = centro.z - p.pos.z;
            float distSq = rx * rx + rz * rz;
            float dist = sqrtf(distSq);

            if (dist <= radioHorizonte || dist > radioMaximo) {
                float r = (float)GetRandomValue(45, 170) / 10.0f;
                float angulo = (float)GetRandomValue(0, 3600) * (3.14159265f / 1800.0f);
                p.pos = { r * cosf(angulo), 0.0f, r * sinf(angulo) };
                float v = sqrtf(masaBH / r);
                p.vel = { -sinf(angulo) * v, 0.0f, cosf(angulo) * v };
            } else {
                float F = (masaBH / (distSq + 0.1f));
                p.vel.x += (rx / dist) * F * dt;
                p.vel.z += (rz / dist) * F * dt;
                p.vel.y -= p.pos.y * 4.0f * dt;

                p.pos.x += p.vel.x * dt;
                p.pos.y += p.vel.y * dt;
                p.pos.z += p.vel.z * dt;
            }
        }

        // --- POSICION EN PANTALLA DEL AGUJERO NEGRO (clave para el shader) ---
        Vector2 bhScreen = GetWorldToScreen(centro, camera);
        SetShaderValue(lenteShader, locBhScreenPos, &bhScreen, SHADER_UNIFORM_VEC2);

        Vector3 bordeHorizonte = { centro.x + radioHorizonte, centro.y, centro.z };
        Vector2 bordeScreen = GetWorldToScreen(bordeHorizonte, camera);
        float dxh = bordeScreen.x - bhScreen.x;
        float dyh = bordeScreen.y - bhScreen.y;
        float horizonPixelRadius = sqrtf(dxh * dxh + dyh * dyh);
        if (horizonPixelRadius < 2.0f) horizonPixelRadius = 2.0f;
        SetShaderValue(lenteShader, locHorizonRadius, &horizonPixelRadius, SHADER_UNIFORM_FLOAT);

        // =============== PASO 1: RENDERIZAR LA ESCENA 3D A TEXTURA ===============
        BeginTextureMode(sceneTarget);
            ClearBackground(BLACK);
            BeginMode3D(camera);

                // 1. POZO GRAVITATORIO EN EMBUDO (MALLA ROJA)
                int numAnillos = 20;
                int numRayos = 32;
                float maxRadio = 18.0f;

                for (int r = 1; r <= numAnillos; r++) {
                    float r1 = (float)(r - 1) * (maxRadio / numAnillos) + 0.6f;
                    float r2 = (float)r * (maxRadio / numAnillos) + 0.6f;

                    float y1 = -8.0f / r1;
                    float y2 = -8.0f / r2;

                    for (int i = 0; i < numRayos; i++) {
                        float a1 = (float)i * (2.0f * 3.14159265f / numRayos);
                        float a2 = (float)(i + 1) * (2.0f * 3.14159265f / numRayos);

                        Vector3 p1 = { r1 * cosf(a1), y1, r1 * sinf(a1) };
                        Vector3 p2 = { r1 * cosf(a2), y1, r1 * sinf(a2) };
                        Vector3 p3 = { r2 * cosf(a1), y2, r2 * sinf(a1) };

                        DrawLine3D(p1, p2, DARKGRAY);
                        DrawLine3D(p1, p3, RED);
                    }
                }

                // 2. CENTRO (AGUJERO NEGRO)
                // DrawSphere solo toma UN radio: (Vector3, float, Color).
                // El shader de post-proceso ya se encarga de curvar
                // visualmente todo lo que hay alrededor.
                DrawSphere(centro, radioHorizonte, BLACK);
                DrawSphereWires(centro, radioHorizonte + 0.01f, 16, 16, RED);

                // 3. DISCO DE ACRECION (con Doppler relativista + beaming)
                for (const auto& p : particulas) {
                    Color colorFinal = AplicarDopplerBeaming(p.pos, p.vel, p.color, camera.position, dopplerEscala);
                    DrawCube(p.pos, p.tamano, p.tamano, p.tamano, colorFinal);
                }

            EndMode3D();
        EndTextureMode();

        // =============== PASO 2: APLICAR EL SHADER DE LENTE Y MOSTRAR ===============
        BeginDrawing();
            ClearBackground(BLACK);

            BeginShaderMode(lenteShader);
                DrawTextureRec(
                    sceneTarget.texture,
                    (Rectangle){ 0, 0, (float)sceneTarget.texture.width, -(float)sceneTarget.texture.height },
                    (Vector2){ 0, 0 },
                    WHITE
                );
            EndShaderMode();

            DrawText("Agujero Negro 3D v3 - Lente + Doppler Relativista", 10, 10, 18, RAYWHITE);
            DrawText(TextFormat("Intensidad de la lente (Flechas Arriba/Abajo): %.2f", lensStrength), 10, 35, 16, RAYWHITE);
            DrawFPS(10, 60);
        EndDrawing();
    }

    UnloadShader(lenteShader);
    UnloadRenderTexture(sceneTarget);
    CloseWindow();
    return 0;
}
// cd C:\msys64\ucrt64\bin
//g++.exe C:\Users\Lenovo\Downloads\agujero_negro_v3.cpp -o C:\Users\Lenovo\Downloads\agujero_negro.exe -lraylib -lopengl32 -lgdi32 -lwinmm
// C:\Users\Lenovo\Downloads
//agujero_negro.exe