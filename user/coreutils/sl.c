/* sl: a steam locomotive crosses the terminal. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

static const char *train[] = {
    "      ====        ________                ___________ ",
    "  _D _|  |_______/        \\__I_I_____===__|_________| ",
    "   |(_)---  |   H\\________/ |   |        =|___ ___|   ",
    "   /     |  |   H  |  |     |   |         ||_| |_||   ",
    "  |      |  |   H  |__--------------------| [___] |   ",
    "  | ________|___H__/__|_____/[][]~\\_______|       |   ",
    "  |/ |   |-----------I_____I [][] []  D   |=======|__ ",
    "__/ =| o |=-~~\\  /~~\\  /~~\\  /~~\\ ____Y___________|__ ",
    " |/-=|___|=    ||    ||    ||    |_____/~\\___/        ",
    "  \\_/      \\O=====O=====O=====O_/      \\_/            ",
};
#define ROWS_T 10
#define TRAIN_W 55

int main(void)
{
    int cols = 80, rows = 25;
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    int top = rows > ROWS_T + 2 ? (rows - ROWS_T) / 2 : 1;
    printf("\033[2J");
    for (int x = cols; x > -TRAIN_W; x--) {
        for (int r = 0; r < ROWS_T; r++) {
            char line[256];
            int n = 0;
            for (int c = 0; c < cols && n < 255; c++) {
                int tx = c - x;
                line[n++] = tx >= 0 && tx < TRAIN_W ? train[r][tx] : ' ';
            }
            line[n] = '\0';
            printf("\033[%d;1H%s", top + r + 1, line);
        }
        fflush(stdout);
        sleep_ms(40);
    }
    printf("\033[2J\033[H");
    return 0;
}
