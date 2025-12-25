#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

static bool g_ShowMouse = false;
static ImVec2 g_MousePos = {0, 0};
static bool g_LeftDown = false, g_RightDown = false;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

static void (*orig_Input1)(void*, void*, void*) = nullptr;
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
    }
    return result;
}

struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst;
    GLboolean blend, cull, depth, scissor;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.aTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDst);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.aTex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bSrc, s.bDst);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static void DrawMouseOverlay() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    
    if (g_ShowMouse) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        
        ImGui::Begin("MouseOverlay", nullptr, 
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 center = ImVec2(g_MousePos.x, g_MousePos.y);
        ImVec2 p1 = ImVec2(center.x - 12, center.y - 12);
        ImVec2 p2 = ImVec2(center.x + 12, center.y + 12);
        
        draw_list->AddRectFilled(p1, p2, IM_COL32(255, 255, 255, g_LeftDown ? 255 : 128));
        draw_list->AddRect(p1, p2, IM_COL32(0, 0, 0, 255), 0.0f, 0, 2.0f);
        
        ImGui::End();
        ImGui::PopStyleVar(3);
    }
    
    ImGui::SetNextWindowPos(ImVec2(g_Width - 200, g_Height - 160));
    ImGui::SetNextWindowSize(ImVec2(180, 140), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("Mouse: %s", g_ShowMouse ? "ON" : "OFF");
    if (ImGui::Button(g_ShowMouse ? "Hide Mouse" : "Show Mouse", ImVec2(-1, 0))) {
        g_ShowMouse = !g_ShowMouse;
    }
    
    ImGui::Separator();
    if (ImGui::Button("Left Click", ImVec2(-1, 0))) {
        g_LeftDown = true;
    }
    if (ImGui::IsItemHovered()) g_LeftDown = false;
    
    if (ImGui::Button("Right Click", ImVec2(-1, 0))) {
        g_RightDown = true;
    }
    if (ImGui::IsItemHovered()) g_RightDown = false;
    
    ImGui::End();
    ImGui::PopStyleVar();
}

static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = (float)g_Height / 720.0f;
    if (scale < 1.5f) scale = 1.5f;
    if (scale > 4.0f) scale = 4.0f;
    ImFontConfig cfg;
    cfg.SizePixels = 18.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::GetStyle().ScaleAllSizes(scale);
    g_Initialized = true;
}

static void Render() {
    if (!g_Initialized) return;
    GLState s;
    SaveGL(s);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawMouseOverlay();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    RestoreGL(s);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = surf;
        }
    }
    if (ctx != g_TargetContext || surf != g_TargetSurface)
        return orig_eglSwapBuffers(dpy, surf);
    g_Width = w;
    g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) {
        GHook h = GlossHook(sym1, (void*)hook_Input1, (void**)&orig_Input1);
        if (h) return;
    }
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) {
        GHook h = GlossHook(sym2, (void*)hook_Input2, (void**)&orig_Input2);
        if (h) return;
    }
}

static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hEGL = GlossOpen("libEGL.so");
    if (!hEGL) return nullptr;
    void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
    if (!swap) return nullptr;
    GHook h = GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    if (!h) return nullptr;
    HookInput();
    return nullptr;
}

__attribute__((constructor))
void MouseOverlay_Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}