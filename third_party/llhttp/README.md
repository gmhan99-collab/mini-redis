# llhttp 9.4.4 (vendored)

| 항목 | 내용 |
|---|---|
| 프로젝트 | llhttp — Node.js HTTP 파서 (https://github.com/nodejs/llhttp) |
| 버전 | 9.4.4 (`LLHTTP_VERSION_MAJOR/MINOR/PATCH` = 9/4/4) |
| 공식 태그 | `release/v9.4.4` (커밋 `816485216511b86459b2e6478190e14c38c14891`) |
| 받은 경로 | https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.4.tar.gz (2026-11-14) |
| 아카이브 SHA-256 | `5600adb1498f82a9c719b8eb19310021737388e0d92362a6d47be532710d3c96` |
| 라이선스 | MIT, Copyright © 2018 Fedor Indutny (`LICENSE`, `LICENSE-MIT`) |
| 수정 여부 | 없음. 릴리스 브랜치의 생성된 C 코드를 그대로 복사 |

## 포함 파일

릴리스 아카이브의 같은 상대 경로에서 수정 없이 복사했습니다.

| 파일 | SHA-256 |
|---|---|
| `include/llhttp.h` | `52a7011f979b31f761403c1a333bf7fc26dc70f5745d8488eed1609c92097100` |
| `src/llhttp.c` | `f3ef756529b7444630717fa5540d4a1c9a1256874bda823c49c5a99c37f89429` |
| `src/http.c` | `a1f2b23168f8e9b5bfa464c89027d3ca259fc649ae3d7e72bfff997d1aeb5d72` |
| `src/api.c` | `0f8590206fe2f264db2825401b5fb856b979ee1363dc88b06f36dc9577afb941` |
| `LICENSE` | `628168d68bb5a8a17e0bbefb3bd74e326e1edc20583d76f52f457c82c921867d` |
| `LICENSE-MIT` | `ebca854e0134cd256d673627c20499f42577eb74bacf08b9f25b626a73c91277` |

## Windows/MinGW 빌드

CMake 없이 세 C 파일을 직접 컴파일합니다. 헤더 경로는 `-Ithird_party/llhttp/include`입니다.

```sh
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Ithird_party/llhttp/include -c third_party/llhttp/src/llhttp.c
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Ithird_party/llhttp/include -c third_party/llhttp/src/http.c
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Ithird_party/llhttp/include -c third_party/llhttp/src/api.c
```

생성 코드는 `-Wextra`에서 `unused parameter` 경고만 냅니다(gcc 16.2.0 MinGW-w64 UCRT에서 확인). 원본을 수정하지 않기 위해 이 세 파일에만 `-Wno-unused-parameter`를 붙이는 것을 권장합니다.

## 사용 방식

`src/server/http_api.c`는 `llhttp_set_lenient_*` 함수를 하나도 호출하지 않습니다. 길이 중복, 잘못된 Content-Length, CR/LF 규칙 위반 같은 모호한 요청은 파서 오류로 거부됩니다.
