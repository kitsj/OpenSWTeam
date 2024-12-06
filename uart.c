#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <wiringPi.h>
#include <softTone.h>
#include <wiringSerial.h>

#define BAUD_RATE 115200

// NFC 및 블루투스 플래그
int nfc_flag = 0;
int bluetooth_flag = 0;
pthread_mutex_t flag_mutex;

static const char* UART2_DEV = "/dev/ttyAMA0"; // UART2
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

// 문자열 전송 함수
void send_message(int fd, const char* msg) {
    while (*msg) {
        serialPutchar(fd, *msg++);
    }
    serialPutchar(fd, '\n'); // 줄바꿈 문자 추가
}

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

// NFC 감지 스레드
void* nfc_task(void* arg) {
    pid_t pid;
    int status;
    char* argv[] = { "nfc-poll", NULL };

    while (1) {
        pthread_mutex_lock(&flag_mutex);
        if (bluetooth_flag == 0 && nfc_flag == 0) { // 블루투스 처리 중에는 NFC 입력 불가
            pthread_mutex_unlock(&flag_mutex);
            printf("NFC 감지 중...\n");
            if (posix_spawn(&pid, "/bin/nfc-poll", NULL, NULL, argv, environ) == 0) {
                if (waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    pthread_mutex_lock(&flag_mutex);
                    nfc_flag = 1; // NFC 인증 성공
                    pthread_mutex_unlock(&flag_mutex);
                    printf("NFC 인증 성공\n");
                }
            } else {
                perror("nfc-poll 실행 실패");
            }
        } else {
            pthread_mutex_unlock(&flag_mutex);
        }
        sleep(1); // NFC 감지 주기
    }
    return NULL;
}

// 블루투스 입력 스레드
void* bluetooth_task(void* arg) {
    int fd = *(int*)arg;
    char buffer[100];
    int index = 0;
    char dat;

    while (1) {
        pthread_mutex_lock(&flag_mutex);
        if (nfc_flag == 1 && bluetooth_flag == 0) { // NFC 인증 후에만 블루투스 활성화
            pthread_mutex_unlock(&flag_mutex);

            // 비밀번호 입력 안내 메시지 전송
            send_message(fd, "비밀번호를 입력해주세요");

            while (1) {
                if (serialDataAvail(fd)) {
                    dat = serialGetchar(fd);
                    if (dat == '\n' || dat == '\r') { // 줄바꿈 문자로 입력 완료 확인
                        buffer[index] = '\0'; // 문자열 종료
                        if (strcmp(buffer, "1234") == 0) { // 비밀번호 검증
                            pthread_mutex_lock(&flag_mutex);
                            bluetooth_flag = 1; // 블루투스 입력 성공
                            pthread_mutex_unlock(&flag_mutex);
                            printf("블루투스 입력 성공\n");
                            break;
                        } else {
                            // 잘못된 비밀번호 처리
                            send_message(fd, "잘못된 비밀번호입니다. 다시 입력해주세요");
                        }
                        // 입력 초기화
                        memset(buffer, '\0', sizeof(buffer)); // 버퍼 초기화
                        index = 0; // 인덱스 초기화
                    } else {
                        if (index < sizeof(buffer) - 1) { // 버퍼 오버플로 방지
                            buffer[index++] = dat;
                        }
                    }
                }
                delay(10);
            }
        } else {
            pthread_mutex_unlock(&flag_mutex);
        }
        delay(10);
    }
    return NULL;
}

// 조건 확인 및 모터 제어 스레드
void* condition_checker(void* arg) {
    while (1) {
        pthread_mutex_lock(&flag_mutex);
        if (nfc_flag == 1 && bluetooth_flag == 1) {
            printf("조건 충족: 모터 작동 시작\n");
            one_two_Phase_Rotate_Angle(45, 1); // 스텝모터 45도 회전
            nfc_flag = 0; // NFC 플래그 초기화 (다시 감지 가능)
            bluetooth_flag = 0; // 블루투스 플래그 초기화
            printf("작업 완료: 새로운 NFC 입력 대기...\n");
        }
        pthread_mutex_unlock(&flag_mutex);
        sleep(1); // 조건 확인 주기
    }
    return NULL;
}

int main() {
    int fd_serial;

    if (wiringPiSetupGpio() < 0) return 1;
    if ((fd_serial = serialOpen(UART2_DEV, BAUD_RATE)) < 0) {
        printf("UART 초기화 실패\n");
        return 1;
    }

    pthread_mutex_init(&flag_mutex, NULL);

    // GPIO 설정
    for (int i = 0; i < 4; i++) {
        pinMode(pin_arr[i], OUTPUT);
    }

    pthread_t nfc_thread, bt_thread, checker_thread;

    // NFC 처리 스레드
    pthread_create(&nfc_thread, NULL, nfc_task, NULL);

    // 블루투스 처리 스레드
    pthread_create(&bt_thread, NULL, bluetooth_task, &fd_serial);

    // 조건 확인 및 모터 제어 스레드
    pthread_create(&checker_thread, NULL, condition_checker, NULL);

    pthread_join(nfc_thread, NULL);
    pthread_join(bt_thread, NULL);
    pthread_join(checker_thread, NULL);

    pthread_mutex_destroy(&flag_mutex);
    serialClose(fd_serial);
    return 0;
}
