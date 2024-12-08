#include <wiringPi.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define BUTTON_GPIO 17 // 실제 GPIO 핀 번호

// 버튼 입력을 처리하는 함수 (스레드용)
void* button_task(void* arg) {
    // 버튼 GPIO 핀 설정
    pinMode(BUTTON_GPIO, INPUT);
    pullUpDnControl(BUTTON_GPIO, PUD_UP);

    while (1) {
        if (digitalRead(BUTTON_GPIO) == LOW) { // 버튼 눌림
            printf("버튼 눌림\n");
            while (digitalRead(BUTTON_GPIO) == LOW) {
                // 버튼이 계속 눌려있으면 중복 출력 방지
                delay(10);
            }
        }
        delay(100); // 버튼 상태 확인 주기
    }
    return NULL;
}

int main(void) {
    pthread_t button_thread; // 버튼 스레드 ID

    // WiringPi 초기화
    if (wiringPiSetupGpio() == -1) {
        printf("WiringPi 초기화 실패\n");
        return 1;
    }

    // 버튼 스레드 생성
    if (pthread_create(&button_thread, NULL, button_task, NULL) != 0) {
        printf("버튼 스레드 생성 실패\n");
        return 1;
    }

    // 메인 프로그램 로직 (예: 대기 상태)
    while (1) {
        printf("메인 프로그램 동작 중...\n");
        sleep(2); // 2초마다 출력
    }

    // 스레드 종료 대기
    pthread_join(button_thread, NULL);

    return 0;
}
