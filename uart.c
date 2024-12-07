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

// 타이머 카운터 전역 변수
#define DAY_TIME 50 // 하루 초기화 시간 (데모용 50초)
#define MAX_COUNT 3
#define INTERVAL_TIME 10 // 약 복용 간격 (초)

int m_count = 0;          // 복용 횟수
time_t last_dose_time;    // 마지막 복용 시간
time_t start_day_time;    // 하루 시작 시간

// 플래그 및 mutex
int nfc_flag = 0;
pthread_mutex_t flag_mutex;

static const char* UART_DEV = "/dev/ttyAMA0"; // UART0
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

void music(int gpio) {
    softToneCreate(gpio);
    for (int i = 0; i < 3; i++) {
        softToneWrite(gpio, 900);
        delay(333);
        softToneWrite(gpio, 0);
        delay(333);
    }
}

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

// 블루투스 입력 함수 (수정 금지)
int bluetooth_input(int fd) {
    char buffer[100]; // 비밀번호 입력 버퍼
    int index = 0;    // 버퍼 인덱스 초기화
    char dat;

    // 비밀번호 입력 안내 메시지 전송
    send_message(fd, "비밀번호를 입력해주세요");

    memset(buffer, '\0', sizeof(buffer)); // 버퍼 명시적으로 초기화

    while (1) {
        if (serialDataAvail(fd)) {
            dat = serialGetchar(fd);
            if (dat == '\n' || dat == '\r') { // 줄바꿈 문자로 입력 완료 확인
                buffer[index] = '\0'; // 문자열 종료
                if (strcmp(buffer, "1234") == 0) { // 비밀번호 검증
                    printf("블루투스 입력 성공\n");
                    return 1; // 성공
                } else {
                    printf("비밀번호 입력\n");
                    memset(buffer, '\0', sizeof(buffer)); // 잘못된 입력 후 버퍼 초기화
                    index = 0; // 인덱스 초기화
                }
            } else {
                if (index < sizeof(buffer) - 1) { // 버퍼 오버플로 방지
                    buffer[index++] = dat;
                }
            }
        }
        delay(10);
    }
    return 0; // 실패
}

// 하루 초기화 스레드
void* daily_reset_task(void* arg) {
    while (1) {
        sleep(1); // 1초 주기로 확인
        pthread_mutex_lock(&flag_mutex);
        time_t now = time(NULL);
        if (difftime(now, start_day_time) >= DAY_TIME) {
            printf("하루가 지나 복용 횟수를 초기화합니다.\n");
            m_count = 0;
            start_day_time = now; // 하루 시작 시간 재설정
        }
        pthread_mutex_unlock(&flag_mutex);
    }
    return NULL;
}

// NFC 감지 스레드
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

                    time_t now = time(NULL);

                    // 복용 간격 및 복용 횟수 확인
                    pthread_mutex_lock(&flag_mutex);
                    if (m_count >= MAX_COUNT) {
                        printf("하루 약 복용 횟수를 초과했습니다. 부저 울림\n");
                        music(18);
                        // NFC 감지가 다시 가능하도록 플래그 초기화
                        nfc_flag = 0;
                        pthread_mutex_unlock(&flag_mutex);
                        continue; // 루프 재시작
                    } else if (difftime(now, last_dose_time) < INTERVAL_TIME) {
                        int remaining = INTERVAL_TIME - (int)difftime(now, last_dose_time);
                        printf("복용 간격 충족되지 않음: %d초 남음. 부저 울림\n", remaining);
                        music(18);

                        // NFC 감지가 다시 가능하도록 플래그 초기화
                        nfc_flag = 0;
                        pthread_mutex_unlock(&flag_mutex);
                        continue; // 루프 재시작
                    } else {
                        pthread_mutex_unlock(&flag_mutex);

                        // NFC 인증 성공 후 블루투스 입력 호출
                        if (bluetooth_input(fd)) {
                            printf("조건 충족: 모터 작동 시작\n");
                            one_two_Phase_Rotate_Angle(45, 1); // 스텝모터 45도 회전

                            pthread_mutex_lock(&flag_mutex);
                            m_count++;               // 복용 횟수 증가
                            last_dose_time = now;    // 마지막 복용 시간 갱신
                            printf("약 복용 횟수 %d\n", m_count);
                            pthread_mutex_unlock(&flag_mutex);
                        }
                    }

                    pthread_mutex_lock(&flag_mutex);
                    nfc_flag = 0; // NFC 플래그 초기화 (다시 감지 가능)
                    pthread_mutex_unlock(&flag_mutex);
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

int main() {
    int fd_serial;

    if (wiringPiSetupGpio() < 0) return 1;
    if ((fd_serial = serialOpen(UART_DEV, BAUD_RATE)) < 0) {
        printf("UART 초기화 실패\n");
        return 1;
    }

    pthread_mutex_init(&flag_mutex, NULL);

    // GPIO 설정
    for (int i = 0; i < 4; i++) {
        pinMode(pin_arr[i], OUTPUT);
    }

    // 초기화
    start_day_time = time(NULL);
    last_dose_time = time(NULL) - INTERVAL_TIME; // 즉시 복용 가능

    pthread_t nfc_thread, reset_thread;

    // 하루 초기화 스레드
    pthread_create(&reset_thread, NULL, daily_reset_task, NULL);

    // NFC 처리 스레드
    pthread_create(&nfc_thread, NULL, nfc_task, &fd_serial);

    pthread_join(reset_thread, NULL);
    pthread_join(nfc_thread, NULL);

    pthread_mutex_destroy(&flag_mutex);
    serialClose(fd_serial);
    return 0;
}
