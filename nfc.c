#include <wiringPi.h>
#include <stdio.h>

// 핀 번호 (WiringPi 기준)
#define BUTTON_PIN 0  // 버튼 연결 핀 (GPIO 17)
#define LED_PIN 1     // LED 연결 핀 (GPIO 18)

int main(void) {
    // WiringPi 초기화
    if (wiringPiSetup() == -1) {
        printf("WiringPi 초기화 실패\n");
        return 1;
    }

    // 핀 모드 설정
    pinMode(BUTTON_PIN, INPUT);  // 버튼 입력
    pinMode(LED_PIN, OUTPUT);   // LED 출력

    // 풀업 저항 활성화 (버튼이 눌리지 않을 때 기본 HIGH로 설정)
    pullUpDnControl(BUTTON_PIN, PUD_UP);

    printf("버튼 입력 대기 중...\n");

    while (1) {
        if (digitalRead(BUTTON_PIN) == LOW) {  // 버튼 눌림 (LOW 상태)
            digitalWrite(LED_PIN, HIGH);      // LED 켜기
            printf("버튼 눌림\n");
        } else {
            digitalWrite(LED_PIN, LOW);       // LED 끄기
        }
        delay(100);  // 0.1초 대기
    }

    return 0;
}
