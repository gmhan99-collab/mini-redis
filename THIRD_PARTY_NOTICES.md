# 외부 코드 출처

## SipHash 참조 구현

- 저장소: https://github.com/veorq/SipHash
- 고정 커밋: `32d067603b93b47828700880649198e0bfbbcffa` (2026-03-07, "Merge pull request #49 from Henriquelay/patch-1")
  - 복사한 소스 파일의 마지막 변경 커밋 시각: 2022-10-22
- 가져온 날짜: 2026-11-13
- 위치: `third_party/siphash/`
- 저작자: Jean-Philippe Aumasson, Daniel J. Bernstein
- 라이선스: CC0 1.0, MIT, Apache 2.0 with LLVM exception 중 선택 가능한 다중 라이선스. 원본 라이선스 파일 세 개를 그대로 함께 둔다.

복사한 파일의 내용은 수정하지 않았고, 원본의 저작권 및 라이선스 주석을 그대로 보존한다.
단, Windows의 Git 체크아웃(줄바꿈 자동 변환)을 거쳐 복사했기 때문에 로컬 파일의 줄바꿈은 원본의 LF가 아니라 CRLF이다.
따라서 아래 SHA-256은 **로컬 CRLF 파일**의 값이며 upstream 원본 파일의 해시와 다르다.
검증 작업에서 고정 커밋의 upstream 파일 6개를 받아 비교한 결과, LF로 정규화한 텍스트는 6개 모두 동일했고 아래 로컬 해시도 6개 모두 일치했다.

| 파일 | 용도 | SHA-256 |
|---|---|---|
| `siphash.c` | SipHash-2-4 계산(라이브러리에 링크) | `ab2e99e71675146c8db6cb4d81bc422f644a2af2509c463bff972dfcf160c9a4` |
| `siphash.h` | `siphash()` 선언 | `09ef0fb72cee12b6fb95b058d668511aba2b6012d06143d3c2863a2a274aa599` |
| `vectors.h` | 공식 테스트 벡터(단위 테스트에서만 사용) | `108162d8cb5f9cad76ca8faba2bc1cecbb875b48880fc6e1116a88c1343c2581` |
| `LICENSE_CC0` | 라이선스 | `084bc2aebf95358b73255bd38213fdb27bfd19c6819ae7c1d0679400cf01cea1` |
| `LICENSE_MIT` | 라이선스 | `bf6f37dce539ffd376034154b8cac60d21fee1331ea4bfb1457a217312509210` |
| `LICENSE_A2LLVM` | 라이선스 | `3ece8a4f4e785cbca185c9bb9f09e6c24a626cb9bdc7a9c7c96cb56609c351fd` |

사용 방식: `siphash.c`를 기본 라운드 수(cROUNDS=2, dROUNDS=4)와 8바이트 출력으로 호출해 SipHash-2-4 64비트 해시를 얻는다(`src/hash_table/key_hash.c`).
정합성 검증: 단위 테스트가 `vectors.h`의 `vectors_sip64` 64개 공식 벡터(키 `00..0f`, 입력 `00..len-1`, len 0~63)와 래퍼 출력을 비교한다.

## llhttp HTTP 파서

- 공식 저장소: https://github.com/nodejs/llhttp
- 버전: **9.4.4**, 태그 `release/v9.4.4`
- 릴리스 소스: https://github.com/nodejs/llhttp/tree/release/v9.4.4
- 위치: `third_party/llhttp/`
- 사용 파일: `src/llhttp.c`, `src/http.c`, `src/api.c`, `include/llhttp.h`
- 라이선스: MIT, Copyright Fedor Indutny, 2018. 원본 라이선스 파일을 함께 보관합니다.
- 생성된 C 소스를 Makefile에서 직접 컴파일하므로 Node.js, CMake, 파서 재생성이 필요하지 않습니다.
- 자체 HTTP 정책과 콜백은 `src/server/http_api.c`에 구현하며 외부 파서의 관대한 옵션을 활성화하지 않습니다.
