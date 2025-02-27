// Dear ImGui: standalone example application for DirectX 11

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <wchar.h> // wprintf

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// TODO(irwin): remove @cleanup
enum Pipe_State
{
    PipeState_Invalid = 0,
    // PipeState_RetryingToOpen,
    PipeState_Open,

    PipeState_COUNT
};


struct Command
{
    bool started;
    bool abort_requested;
    HANDLE stdout_read;
    HANDLE stdout_write;
    PROCESS_INFORMATION process_information;
    Pipe_State pipe_state;
};


void PrintLastError(DWORD error_code)
{
    wchar_t *buffer;
    DWORD format_result = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
        0,
        error_code,
        0,
        (LPWSTR)&buffer,
        0,
        0
    );
    if (format_result)
    {
        wprintf(buffer);
        LocalFree(buffer);
    }
}

void Win32OutputLastError()
{
    PrintLastError(GetLastError());
}



// NOTE(irwin): caller must free
int UTF8_ToWidechar(wchar_t **dest, const char *str, size_t str_size)
{
    int buf_char_count_needed = MultiByteToWideChar(
        CP_UTF8,
        0,
        str,
        (int)str_size,
        0,
        0
    );

    if (buf_char_count_needed)
    {
        *dest = (wchar_t *)malloc((buf_char_count_needed + 1) * sizeof(wchar_t));
        MultiByteToWideChar(
            CP_UTF8,
            0,
            str,
            (int)str_size,
            *dest,
            buf_char_count_needed
        );

        (*dest)[buf_char_count_needed] = 0;
    }

    return buf_char_count_needed;
}

int UTF8_ToWidechar(wchar_t **dest, const char *str)
{
    size_t len = 0;
    if (str)
    {
        len = strlen(str);
    }
    return UTF8_ToWidechar(dest, str, len);
}

// TODO(irwin): this parser sucks, rewrite or switch to parsing ripgrep's json output, we'll
//              probably have to do that anyway if we want to highlight matches inline
enum Token_Kind
{
    Token_Kind_Invalid = 0,
    Token_Kind_Colon,
    Token_Kind_CR,
    Token_Kind_LF,
    Token_Kind_Rest,

    Token_Kind_COUNT
};

struct Token
{
    int index;
    int ch;
    Token_Kind previous_kind;
    Token_Kind kind;
    Token_Kind next_kind;
};

static inline Token_Kind get_token_kind(int ch)
{
    // TODO(irwin): we probably want to skip non-printable characters, but I don't know
    // if this check will fail correct utf8 characters or bytes, since atm the incoming
    // text encoding is uncertain, so assume it's ascii for now
    // if (ch > 0 && ch < 255)
    if (!!true)
    {
        switch (ch)
        {
            case ':':
            {
                return Token_Kind_Colon;
            } break;

            case '\n':
            {
                return Token_Kind_LF;
            } break;

            case '\r':
            {
                return Token_Kind_CR;
            } break;

            default:
            {
                return Token_Kind_Rest;
            } break;
        }
    }
    else
    {
        return Token_Kind_Invalid;
    }
}

Token get_token_at_index(ImGuiTextBuffer *ripgrep_output, int index)
{
    Token token = {0};
    token.index = index;

    if (index < ripgrep_output->size())
    {
        token.ch = (*ripgrep_output)[index];
        token.kind = get_token_kind(token.ch);
        token.previous_kind = index > 0 ? get_token_kind((*ripgrep_output)[index-1]) : Token_Kind_Invalid;
        token.next_kind = (index + 1) < ripgrep_output->size() ? get_token_kind((*ripgrep_output)[index+1]) : Token_Kind_Invalid;
    }

    return token;
}

struct IndexedString
{
    int first;
    int one_past_last;
};

struct ParsedLine
{
    IndexedString filepath;
    IndexedString line_number;
    IndexedString match;
};

static Token eat_all_newlines(ImGuiTextBuffer *ripgrep_output, Token at)
{
    while (at.kind != Token_Kind_Invalid &&
           (at.kind == Token_Kind_CR || at.kind == Token_Kind_LF))
    {
        at = get_token_at_index(ripgrep_output, at.index+1);
    }

    return at;
}

static Token eat_until_newlines(ImGuiTextBuffer *ripgrep_output, Token at)
{
    while (at.kind != Token_Kind_Invalid &&
           at.kind != Token_Kind_CR &&
           at.kind != Token_Kind_LF)
    {
        at = get_token_at_index(ripgrep_output, at.index+1);
    }

    return at;
}

static Token eat_until_token(ImGuiTextBuffer *ripgrep_output, int start_index, Token_Kind kind)
{
    IM_ASSERT(kind >= Token_Kind_Invalid && kind < Token_Kind_COUNT);

    Token start = get_token_at_index(ripgrep_output, start_index);
    while (start.kind != Token_Kind_Invalid && start.kind != kind)
    {
        start = get_token_at_index(ripgrep_output, start.index+1);
    }

    return start;
}

static IndexedString parse_filepath(ImGuiTextBuffer *ripgrep_output, Token at)
{
    Token start = eat_all_newlines(ripgrep_output, at);
    Token last = eat_until_token(ripgrep_output, start.index, Token_Kind_Colon);
    if (last.kind == Token_Kind_Colon && last.index - start.index == 1)
    {
        last = eat_until_token(ripgrep_output, last.index+1, Token_Kind_Colon);
    }

    // IM_ASSERT(start.index < last.index);

    IndexedString filepath = {0};
    filepath.first = start.index;
    filepath.one_past_last = last.index;

    return filepath;
}

static IndexedString parse_line_num(ImGuiTextBuffer *ripgrep_output, Token at)
{
    Token start = eat_all_newlines(ripgrep_output, at);
    start = eat_until_token(ripgrep_output, start.index, Token_Kind_Colon);
    Token last = {0};
    if (start.kind == Token_Kind_Colon)
    {
        last = eat_until_token(ripgrep_output, start.index+1, Token_Kind_Colon);
    }

    IndexedString line_number = {0};
    line_number.first = start.index+1;
    line_number.one_past_last = last.index;

    return line_number;
}

static IndexedString parse_match(ImGuiTextBuffer *ripgrep_output, Token at)
{
    Token start = get_token_at_index(ripgrep_output, at.index+1);
    Token last = eat_until_newlines(ripgrep_output, start);

    // IM_ASSERT(start.index < last.index);

    IndexedString match = {0};
    match.first = start.index;
    match.one_past_last = last.index;

    return match;
}

#if 0
#else

static int parse_rg_stdout(ImGuiTextBuffer *ripgrep_output, ImVector<ParsedLine> *lines)
{
    int first_index = 0;
    int lines_count_before = lines->size();
    if (lines_count_before > 0)
    {
        // NOTE(irwin): one_past_last is at LF
        first_index = lines->back().match.one_past_last;
    }

    for (int char_index = first_index; char_index < ripgrep_output->size();)
    {
        Token token = get_token_at_index(ripgrep_output, char_index);

        IndexedString filepath = parse_filepath(ripgrep_output, token);
        token = get_token_at_index(ripgrep_output, filepath.one_past_last);
        if (token.kind == Token_Kind_Colon)
        {
            IndexedString line_number = parse_line_num(ripgrep_output, token);
            token = get_token_at_index(ripgrep_output, line_number.one_past_last);
            if (token.kind == Token_Kind_Colon)
            {
                IndexedString match = parse_match(ripgrep_output, token);
                token = get_token_at_index(ripgrep_output, match.one_past_last);

                if (token.kind == Token_Kind_CR || token.kind == Token_Kind_LF)
                {
                    if (filepath.first < filepath.one_past_last &&
                        line_number.first < line_number.one_past_last &&
                        match.first < match.one_past_last)
                    {
                        ParsedLine new_line = {0};
                        new_line.filepath = filepath;
                        new_line.line_number = line_number;
                        new_line.match = match;
                        lines->push_back(new_line);

                        char_index = new_line.match.one_past_last;
                    }
                    else
                    {
                        break;
                    }
                }
                else
                {
                    break;
                }
            }
            else
            {
                break;
            }
        }
        else
        {
            break;
        }
    }

    return lines->size() - lines_count_before;
}
#endif

void kill_running_command(Command *command)
{
    TerminateProcess(command->process_information.hProcess, 0);
    command->started = false;
    *command = {0};
}

// Main code
int main(int, char**)
{
    // Create application window
    //ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"barerg", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"barerg - barebones ripgrep ui", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
    //io.ConfigViewportsNoAutoMerge = true;
    //io.ConfigViewportsNoTaskBarIcon = true;
    //io.ConfigViewportsNoDefaultParent = true;
    //io.ConfigDockingAlwaysTabBar = true;
    //io.ConfigDockingTransparentPayload = true;
    //io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;     // FIXME-DPI: Experimental. THIS CURRENTLY DOESN'T WORK AS EXPECTED. DON'T USE IN USER APP!
    //io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports; // FIXME-DPI: Experimental.

    // Setup Dear ImGui style
    // ImGui::StyleColorsDark();
    ImGui::StyleColorsLight();
    ImGui::GetStyle().FrameBorderSize = 1.0f;
    io.Fonts->AddFontFromFileTTF("c:/Windows/Fonts/segoeui.ttf", 18.0f);
    // io.Fonts->AddFontFromFileTTF("c:/Windows/Fonts/segoeui.ttf", 22.0f);


    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != nullptr);

    // Our state
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    Command command = {0};
    static ImGuiTextBuffer ripgrep_output;
    static ImVector<ParsedLine> ripgrep_output_lines;

    float smoothed_framerate = io.Framerate;

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);

    long long next_search_schedule_when_typing = LLONG_MAX;

    // Main loop
    bool done = false;
    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window being minimized or screen locked
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        static bool was_active = false;

        static int activeFrames = 3;
        if( command.started || was_active )
        {
            activeFrames = 3;
        }
        else
        {
            auto ctx = ImGui::GetCurrentContext();
            if( ctx->NavWindowingTarget || ( ctx->DimBgRatio != 0 && ctx->DimBgRatio != 1 ) )
            {
                activeFrames = 3;
            }
            else
            {
                auto& inputQueue = ctx->InputEventsQueue;
                if( !inputQueue.empty() )
                {
                    for( auto& v : inputQueue )
                    {
                        if( v.Type != ImGuiInputEventType_MouseViewport )
                        {
                            activeFrames = 3;
                            break;
                        }
                    }
                }
            }
        }
        if( activeFrames == 0 )
        {
            // std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
            ::Sleep(16);
            continue;
        }
        activeFrames--;
        was_active = false;




        ImGui::NewFrame();

        ImGuiViewport *viewport = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);

        float alpha = 0.0625f;
        smoothed_framerate = smoothed_framerate * (1.0f - alpha) + io.Framerate * alpha;

        // 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
        {
            ImGui::Begin("Hello, world!", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_MenuBar);

            ImGui::BeginMenuBar();
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / smoothed_framerate, smoothed_framerate);
            ImGui::EndMenuBar();

            {
                if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F) && !ImGui::IsAnyItemActive())
                {
                    ImGui::SetKeyboardFocusHere();
                }
                static char ripgrep_query[1024] = "imgui";
                bool run_pressed = false;
                if(ImGui::InputText("ripgrep_query", ripgrep_query, IM_ARRAYSIZE(ripgrep_query)))
                {
                    LARGE_INTEGER current_timestamp;
                    QueryPerformanceCounter(&current_timestamp);

                    next_search_schedule_when_typing = current_timestamp.QuadPart + (frequency.QuadPart >> 2);
                }
                else
                {
                    // run_pressed |= ImGui::IsItemDeactivatedAfterEdit();
                }
                was_active |= ImGui::IsItemActive();

                static char ripgrep_dir[1024] = "c:\\proj\\cpp";
                ImGui::InputText("ripgrep_dir", ripgrep_dir, IM_ARRAYSIZE(ripgrep_dir));
                was_active |= ImGui::IsItemActive();
                run_pressed |= ImGui::IsItemDeactivatedAfterEdit();

                // TODO(irwin): when we have separate input field for search term, restart search if time since last input
                //              change is greater than ~20-100 ms
                static bool ignore_case = false;
                run_pressed |= ImGui::Checkbox("ignore_case", &ignore_case);
                ImGui::SameLine();

                static char ripgrep_search_command[1024] = "rg.exe --line-number";
                ImGui::InputText("ripgrep_search_command", ripgrep_search_command, IM_ARRAYSIZE(ripgrep_search_command));
                was_active |= ImGui::IsItemActive();
                run_pressed |= ImGui::IsItemDeactivatedAfterEdit();

                run_pressed |= ImGui::Button("Run ripgrep");
                ImGui::SameLine();
                ImGui::BeginDisabled(!command.started);
                if(ImGui::Button("Stop"))
                {
                    kill_running_command(&command);
                }
                ImGui::EndDisabled();


                ImGui::SameLine();
                ImGui::Text("%d matches", ripgrep_output_lines.size());

                // TODO(irwin): extract start/kill helpers

                LARGE_INTEGER current_timestamp;
                QueryPerformanceCounter(&current_timestamp);
                if (run_pressed || current_timestamp.QuadPart >= next_search_schedule_when_typing)
                {
                    next_search_schedule_when_typing = LLONG_MAX;
                    if (command.started)
                    {
                        kill_running_command(&command);
                    }
                    ripgrep_output.clear();
                    ripgrep_output_lines.clear();

                    SECURITY_ATTRIBUTES saAttr;
                    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
                    saAttr.bInheritHandle = TRUE;
                    saAttr.lpSecurityDescriptor = NULL;

                    if (!CreatePipe(&command.stdout_read, &command.stdout_write, &saAttr, 0))
                    {
                        Win32OutputLastError();
                    }
                    else
                    {
                        if (!SetHandleInformation(command.stdout_read, HANDLE_FLAG_INHERIT, 0))
                        {
                            Win32OutputLastError();
                        }
                        else
                        {
                            STARTUPINFOW si = { sizeof(si) };
                            si.hStdOutput = command.stdout_write;
                            si.dwFlags |= STARTF_USESTDHANDLES;

                            command.process_information = {};

                            // const char command_string[] = "rg.exe imgui c:\\proj\\cpp";
                            // const char command_string[] = "cmd /c echo hello";
                            const char working_dir[] = "c:\\proj\\cpp";

                            ImGuiTextBuffer command_builder;
                            command_builder.append(ripgrep_search_command);
                            if (ignore_case)
                            {
                                command_builder.append(" -i");
                            }
                            command_builder.appendf(" %s", ripgrep_query);
                            command_builder.appendf(" %s", ripgrep_dir);

                            wchar_t *command_string_wide = 0;
                            UTF8_ToWidechar(&command_string_wide, command_builder.c_str());

                            if (command_string_wide)
                            {
                                wchar_t *working_dir_wide = 0;
                                UTF8_ToWidechar(&working_dir_wide, working_dir);
                                if (CreateProcessW(0, command_string_wide, 0, 0, true, CREATE_NO_WINDOW, 0, 0, &si, &command.process_information) != 0)
                                {
                                    command.started = true;
                                    CloseHandle(command.stdout_write);
                                }
                                else
                                {
                                    // TODO(irwin): we need to remove broken command if we don't want it to be retried ad infinitum
                                    // TODO(irwin): run CompleteExecutingCommand ?
                                    CloseHandle(command.stdout_read);
                                    CloseHandle(command.stdout_write);
                                }
                                if (working_dir_wide)
                                {
                                    free(working_dir_wide);
                                }
                                free(command_string_wide);
                            }

                        }


                    }

                }
                // if (!ripgrep_output_lines.empty())
                {
                    // if (ImGui::BeginChild("ripgrep output"))
                    {
                        static ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

                        // PushStyleCompact();
                        // ImGui::CheckboxFlags("ImGuiTableFlags_ScrollY", &flags, ImGuiTableFlags_ScrollY);
                        // PopStyleCompact();

                        // const float TEXT_BASE_WIDTH = ImGui::CalcTextSize("A").x;
                        // const float TEXT_BASE_HEIGHT = ImGui::GetTextLineHeightWithSpacing();
                        // IM_UNUSED(TEXT_BASE_WIDTH);


                        // When using ScrollX or ScrollY we need to specify a size for our table container!
                        // Otherwise by default the table will fit all available space, like a BeginChild() call.
                        ImVec2 outer_size = ImVec2(0.0f, 0.0f);
                        if (ImGui::BeginTable("ripgrep_table", 4, flags, outer_size))
                        {
                            ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible
                            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_None);
                            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_None);
                            ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_None);
                            ImGui::TableSetupColumn("Match", ImGuiTableColumnFlags_None);
                            // ImGui::TableSetupColumn("Three", ImGuiTableColumnFlags_None);
                            ImGui::TableHeadersRow();

                            // if (ripgrep_output_lines.empty())
                            // {
                            //     ImGui::TableNextRow();
                            // }

                            // Demonstrate using clipper for large vertical lists
                            ImGuiListClipper clipper;
                            clipper.Begin(ripgrep_output_lines.size());
                            while (clipper.Step())
                            {
                                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                                {
                                    ParsedLine line = ripgrep_output_lines[row];
                                    ImGui::TableNextRow();
#if 0
#else

                                    ImGui::TableSetColumnIndex(0);
                                    ImGui::Text("%d", row+1);

                                    bool pressed = false;
                                    ImGui::TableSetColumnIndex(1);
                                    {
                                        ImGuiTextBuffer filepath;
                                        filepath.append(ripgrep_output.begin() + line.filepath.first, ripgrep_output.begin() + line.filepath.one_past_last);
                                        ImGui::PushID(row);
                                        pressed = ImGui::Selectable(filepath.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap);
                                        if (ImGui::BeginPopupContextItem())
                                        {
                                            if (ImGui::MenuItem("Copy row"))
                                            {
                                                ImGuiTextBuffer to_copy;
                                                to_copy.append(ripgrep_output.begin() + line.filepath.first, ripgrep_output.begin() + line.filepath.one_past_last);
                                                to_copy.appendf("%s", ":");
                                                to_copy.append(ripgrep_output.begin() + line.line_number.first, ripgrep_output.begin() + line.line_number.one_past_last);
                                                to_copy.appendf("%s", ":");
                                                to_copy.append(ripgrep_output.begin() + line.match.first, ripgrep_output.begin() + line.match.one_past_last);
                                                ImGui::SetClipboardText(to_copy.c_str());
                                            }
                                            ImGui::EndPopup();
                                        }
                                        ImGui::PopID();
                                    }

                                    ImGui::TableSetColumnIndex(2);
                                    {
                                        const char *line_first = ripgrep_output.begin() + line.line_number.first;
                                        const char *line_one_past_last = ripgrep_output.begin() + line.line_number.one_past_last;
                                        // ImGui::SetNextItemWidth(-ImGui::CalcTextSize(line_first, line_one_past_last).x);
                                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(line_first, line_one_past_last).x) - 3.0f);

                                        ImGuiTextBuffer buf;
                                        buf.append(line_first, line_one_past_last);
                                        // ImGui::SetNextItemWidth(-ImGui::GetContentRegionAvail().x);
                                        // ImGui::SetNextItemWidth(-FLT_MIN);
                                        // ImGui::SetNextItemWidth(-100.0f);
                                        ImGui::Text(buf.c_str());

                                        if (pressed)
                                        {
                                            buf.clear();
                                            buf.append("C:\\Program Files (x86)\\Notepad++\\notepad++.exe");

                                            ImGuiTextBuffer buf2;
                                            buf2.appendf("\"%.*s\"", line.filepath.one_past_last - line.filepath.first, ripgrep_output.begin() + line.filepath.first);
                                            buf2.appendf(" -n%.*s", line.line_number.one_past_last - line.line_number.first, ripgrep_output.begin() + line.line_number.first);

                                            ShellExecuteA(NULL, "open", buf.c_str(), buf2.c_str(), NULL, 0);
                                        }
                                    }
#endif

                                    // ImGui::TextUnformatted(line_first, line_one_past_last);
                                    // ImGui::Text("%.*s", line_one_past_last - line_first, line_first);


                                    ImGui::TableSetColumnIndex(3);
                                    ImGui::TextUnformatted(ripgrep_output.begin() + line.match.first, ripgrep_output.begin() + line.match.one_past_last);
                                    if (ImGui::BeginItemTooltip())
                                    {
                                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                                        ImGui::TextUnformatted(ripgrep_output.begin() + line.match.first, ripgrep_output.begin() + line.match.one_past_last);
                                        ImGui::PopTextWrapPos();
                                        ImGui::EndTooltip();
                                    }
                                    else if (ImGui::IsItemHovered())
                                    {
                                        was_active |= true;
                                    }

                                }
                            }
                            ImGui::EndTable();
                        }
                    }
                    // ImGui::EndChild();
                }
                // else
                {
                    // ImGui::Text("Ripgrep output empty");
                }
            }

            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        if (command.started)
        {
            bool first_bytes_arrived = !ripgrep_output.empty();
            if (!first_bytes_arrived)
            {
                DWORD bytes_available = 0;
                BOOL peek_success = PeekNamedPipe(
                    command.stdout_read,      //HANDLE  hNamedPipe,
                    NULL,             //LPVOID  lpBuffer,
                    0,                //DWORD   nBufferSize,
                    NULL,             //LPDWORD lpBytesRead,
                    &bytes_available, //LPDWORD lpTotalBytesAvail,
                    NULL              //LPDWORD lpBytesLeftThisMessage
                );
                IM_UNUSED(peek_success);

                first_bytes_arrived = bytes_available > 0;
            }

            // NOTE(irwin): prevent ReadFile ui lag on process creation
            if (first_bytes_arrived)
            {
                DWORD read;
                const int BUFSIZE = 4096;
                char chBuf[BUFSIZE] = {};

                if (ReadFile(command.stdout_read, chBuf, BUFSIZE, &read, NULL))
                {
                    ripgrep_output.append(&chBuf[0], &chBuf[0] + read);
                    int lines_parsed = parse_rg_stdout(&ripgrep_output, &ripgrep_output_lines);
                    IM_UNUSED(lines_parsed);
                }
                else
                {
                    command.started = false;
                    CloseHandle(command.stdout_read);
                    {
                    }
                }
            }

        }


        // Present
        HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
        //HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helper functions
bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0 // From Windows SDK 8.1+ headers
#endif

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_DPICHANGED:
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DpiEnableScaleViewports)
        {
            //const int dpi = HIWORD(wParam);
            //printf("WM_DPICHANGED to %d (%.0f%%)\n", dpi, (float)dpi / 96.0f * 100.0f);
            const RECT* suggested_rect = (RECT*)lParam;
            ::SetWindowPos(hWnd, nullptr, suggested_rect->left, suggested_rect->top, suggested_rect->right - suggested_rect->left, suggested_rect->bottom - suggested_rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        break;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
