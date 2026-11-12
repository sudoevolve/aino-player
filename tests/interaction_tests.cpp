// 不创建窗口，直接重现控件消失、坐标变化和失去鼠标捕获后的输入生命周期。
#define AINO_IMPLEMENTATION
#include "../src/aino.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        aino::Ui ui;
        auto& input = ui.impl->input;
        const aino::Rect box{0, 0, 100, 30};
        auto frame = [&] { ui.begin_frame(400, 300, .016f, 1); };
        auto click = [&](uint64_t id) {
            input.mouse = {10, 10}; input.down[0] = true; input.pressed[0] = true;
            frame(); check(aino::detail::interact(ui.impl, id, box, true).pressed, "next button must accept press"); ui.end_frame();
            input.down[0] = false; input.released[0] = true;
            frame(); check(aino::detail::interact(ui.impl, id, box, true).clicked, "next button must accept click"); ui.end_frame();
        };
        for (const bool release_delivered : {true, false}) {
            input.mouse = {10, 10}; input.down[0] = true; input.pressed[0] = true;
            frame(); check(aino::detail::interact(ui.impl, 1, box, true).pressed, "fixture starts active control"); ui.end_frame();
            input.down[0] = false; input.released[0] = release_delivered;
            frame();
            // 原按钮已被刷新掉，另一个控件不能替原按钮清理 active。
            aino::detail::interact(ui.impl, 2, {100, 100, 100, 30}, true);
            ui.end_frame();
            check(ui.impl->active == 0, "removed control releases capture even without a delivered mouse-up");
            click(3);
        }
        // 正常跨帧拖动不能被帧末清理打断。
        input.down[0] = true; input.pressed[0] = true;
        frame(); aino::detail::interact(ui.impl, 4, box, true); ui.end_frame();
        frame(); check(aino::detail::interact(ui.impl, 4, box, true).held, "drag remains held while mouse is down"); ui.end_frame();
        input.down[0] = false; input.released[0] = true;
        frame(); check(aino::detail::interact(ui.impl, 4, box, true).clicked, "drag releases normally"); ui.end_frame();
        click(5);
        std::cout << "UI capture lifecycle regression checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
