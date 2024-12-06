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

// 전역 변수
int max_count = 3;      // 하루 최대 복용 횟수
int max_time = 10;      // 최대 대기 시간 (초 단위)
int today_count = 0;    // 오늘 복용 횟수
int timer_flag = 0;     // 타이머 상태 (1: 대기 완료, 0: 대기 중)

// 플래그 및 mutex
int nfc_flag = 0;       // NFC 인증 플래그
pthread_mutex_t flag_mutex;
pthread_mutex_t timer_mutex;

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

// 음악 알림 함수
void music(int gpio) {
    softToneCreate(gpio);
    for (int i = 0; i < 3; i++) {
        softToneWrite(gpio, 900);
        delay(333);
        softToneWrite(gpio, 0);
        delay(333);
    }
}

// 블루투스 입력 함수
int bluetooth_input(int fd) {
    char buffer[100]; // 비밀번호 입력 버퍼
    int index = 0;    // 버퍼 인덱스 초기화
    char dat;

    // 비밀번호 입력 안내 메시지 전송
    send_message(fd, "비밀번호를 입력해주세요");

    memset(buffer, '\0', sizeof(buffer)); // 버퍼 초기화

    while (1) {
        if (serialDataAvail(fd)) {
            dat = serialGetchar(fd);
            if (dat == '\n' || dat == '\r') { // 줄바꿈 문자로 입력 완료 확인
                buffer[index] = '\0'; // 문자열 종료
                if (strcmp(buffer, "1234") == 0) { // 비밀번호 검증
                    printf("블루투스 입력 성공\n");
                    return 1; // 성공
                } else {
                    printf("잘못된 비밀번호 입력\n");
                    send_message(fd, "잘못된 비밀번호입니다. 다시 입력해주세요");
                    memset(buffer, '\0', sizeof(buffer)); // 버퍼 초기화
                    index = 0; // 인덱스 초기화
                }
            } else {
                if (index < sizeof(buffer) - 1) {
                    buffer[index++] = dat;
                }
            }
        }
        delay(10);
    }
    return 0;
}

// 타이머 스레드
void* timer_task(void* arg) {
    while (1) {
        pthread_mutex_lock(&timer_mutex);
        if (timer_flag == 0) { // 타이머 대기 중일 때만 작동
            printf("타이머 시작: %d초 대기 중...\n", max_time);
            sleep(max_time); // 대기 시간
            timer_flag = 1; // 대기 완료 상태 설정
            printf("타이머 완료: 복용 가능 상태\n");
        }
        pthread_mutex_unlock(&timer_mutex);
        sleep(1);
    }
    return NULL;
}

// NFC 태그 처리 스레드
void* nfc_task(void* arg) {
    pid_t pid;
    int status;
    char* argv[] = { "nfc-poll", NULL };
    int fd = *(int*)arg;

    while (1) {
        pthread_mutex_lock(&flag_mutex);
        if (nfc_flag == 0) {
            pthread_mutex_unlock(&flag_mutex);
            printf("NFC 감지 중...\n");
            if (posix_spawn(&pid, "/bin/nfc-poll", NULL, NULL, argv, environ) == 0) {
                if (waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    pthread_mutex_lock(&flag_mutex);
                    nfc_flag = 1; // NFC 인증 성공
                    pthread_mutex_unlock(&flag_mutex);
                    printf("NFC 인증 성공\n");

                    pthread_mutex_lock(&timer_mutex);
                    if (timer_flag == 1 && today_count < max_count) { // 타이머 완료 및 복용 가능 상태
                        timer_flag = 0; // 타이머 리셋
                        pthread_mutex_unlock(&timer_mutex);

                        if (bluetooth_input(fd)) { // 비밀번호 입력 성공 시
                            printf("조건 충족: 모터 작동\n");
                            one_two_Phase_Rotate_Angle(45, 1); // 스텝모터 45도 회전
                            pthread_mutex_lock(&timer_mutex);
                            today_count++; // 복용 횟수 증가
                            printf("복용 완료: 오늘 복용 횟수 = %d\n", today_count);
                            pthread_mutex_unlock(&timer_mutex);
                        }
                    } else {
                        pthread_mutex_unlock(&timer_mutex);
                        printf("복용 제한: 부저 울림\n");
                        music(18); // 복용 제한 시 알림음 출력
                    }

                    pthread_mutex_lock(&flag_mutex);
                    nfc_flag = 0; // NFC 플래그 초기화
                    pthread_mutex_unlock(&flag_mutex);
                }
            } else {
                perror("nfc-poll 실행 실패");
            }
        } else {
            pthread_mutex_unlock(&flag_mutex);
        }
        sleep(1);
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
    pthread_mutex_init(&timer_mutex, NULL);

    // GPIO 설정
    for (int i = 0; i < 4; i++) {
        pinMode(pin_arr[i], OUTPUT);
    }

    pthread_t nfc_thread, timer_thread;

    // NFC 처리 스레드
    pthread_create(&nfc_thread, NULL, nfc_task, &fd_serial);

    // 타이머 스레드
    pthread_create(&timer_thread, NULL, timer_task, NULL);

    pthread_join(nfc_thread, NULL);
    pthread_join(timer_thread, NULL);

    pthread_mutex_destroy(&flag_mutex);
    pthread_mutex_destroy(&timer_mutex);
    serialClose(fd_serial);
    return 0;
}
