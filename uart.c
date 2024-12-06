#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <wiringPi.h>
#include <softTone.h>

#define BAUD_RATE 115200
#define GPIO1 18

// NFC 및 블루투스 플래그
int nfc_flag = 0;
int bluetooth_flag = 0;
pthread_mutex_t flag_mutex;

static const char* UART2_DEV = "/dev/ttyAMA2"; // UART2
extern char** environ;

// 스텝모터 관련 GPIO 및 설정
int pin_arr[4] = { 12, 16, 20, 21 };
int one_phase[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

// 스텝모터 동작 함수
void one_two_Phase_Rotate_Angle(float angle, int dir) {
    int steps = angle * (4096 / 360);
    for (int i = 0; i < steps; i++) {
        for (int j = 0; j < 4; j++) {
            digitalWrite(pin_arr[j], one_phase[dir == 1 ? i % 8 : (7 - i % 8)][j]);
        }
        delay(2);
    }
}

// NFC 인증 함수
int nfc_detect() {
    pid_t pid;
    int status;
    char* argv[] = { "nfc-poll", NULL };
    
    printf("NFC 감지 중...\n");
    if (posix_spawn(&pid, "/bin/nfc-poll", NULL, NULL, argv, environ) != 0) {
        perror("nfc-poll 실행 실패");
        return 0;
    }

    if (waitpid(pid, &status, 0) < 0) {
        perror("nfc-poll 대기 실패");
        return 0;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        pthread_mutex_lock(&flag_mutex);
        nfc_flag = 1; // NFC 인증 성공
        pthread_mutex_unlock(&flag_mutex);
        printf("NFC 인증 성공\n");
        return 1;
    }
    return 0;
}

// 블루투스 데이터 처리 스레드
void* bluetooth_listener(void* arg) {
    int fd = *(int*)arg;
    char buffer[100];
    int index = 0;
    char dat;

    while (1) {
        if (serialDataAvail(fd)) {
            dat = serialGetchar(fd);
            if (dat == '\n' || dat == '\r') {
                buffer[index] = '\0';
                if (strcmp(buffer, "1234") == 0) {
                    pthread_mutex_lock(&flag_mutex);
                    bluetooth_flag = 1; // 블루투스 입력 성공
                    pthread_mutex_unlock(&flag_mutex);
                    printf("블루투스 입력 성공\n");
                }
                memset(buffer, '\0', sizeof(buffer));
                index = 0;
            } else {
                buffer[index++] = dat;
            }
        }
        delay(10);
    }
}

// 조건 확인 및 스텝모터 제어
void* condition_checker(void* arg) {
    while (1) {
        pthread_mutex_lock(&flag_mutex);
        if (nfc_flag == 1 && bluetooth_flag == 1) {
            printf("조건 충족: 모터 작동 시작\n");
            one_two_Phase_Rotate_Angle(45, 1); // 스텝모터 45도 회전
            nfc_flag = 0; // 상태 초기화
            bluetooth_flag = 0;
        }
        pthread_mutex_unlock(&flag_mutex);
        sleep(1);
    }
}

int main() {
    int fd_serial;

    if (wiringPiSetupGpio() < 0) return 1;
    if ((fd_serial = serialOpen(UART2_DEV, BAUD_RATE)) < 0) {
        printf("Unable to open serial device.\n");
        return 1;
    }

    pthread_mutex_init(&flag_mutex, NULL);

    // GPIO 설정
    for (int i = 0; i < 4; i++) {
        pinMode(pin_arr[i], OUTPUT);
    }

    pthread_t nfc_thread, bt_thread, checker_thread;

    // NFC 처리 스레드
    pthread_create(&nfc_thread, NULL, (void* (*)(void*))nfc_detect, NULL);

    // 블루투스 처리 스레드
    pthread_create(&bt_thread, NULL, bluetooth_listener, &fd_serial);

    // 조건 확인 스레드
    pthread_create(&checker_thread, NULL, condition_checker, NULL);

    pthread_join(nfc_thread, NULL);
    pthread_join(bt_thread, NULL);
    pthread_join(checker_thread, NULL);

    pthread_mutex_destroy(&flag_mutex);
    return 0;
}
