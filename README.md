# 개요
- Windows 환경에서 동작하는 마크다운(`.md`) 뷰어 및 문서 변환 애플리케이션.
- C 언어로 작성되었으며, Win32 API 및 GDI를 직접 이용.
- CRT를 정적 링크(`/MT`)하여 외부 런타임(JRE, Python, Pandoc, MS Office 등)이나 VC++ 재배포 패키지의 별도 설치 없이 단독 실행 가능.
- 마크다운 문서를 열어 화면에 렌더링하고, PDF 및 Word(`.docx`) 형식으로 내보내기 지원.

# 빌드
- `build.bat` 스크립트를 통해 빌드.
  - MSVC 64비트 컴파일러(`cl.exe`), 리소스 컴파일러(`rc.exe`), 링커(`link.exe`) 필요.
  - `build.bat [debug]` 형태로 실행 시 디버그 빌드 가능.
- `icon.ico` 파일이 프로젝트 루트에 있는 경우 자동으로 `res/icon.ico`에 동기화하여 리소스에 포함.
- 빌드 결과물은 `build/Lilac.exe`.

# 실행 및 파일 열기
- GUI 실행:
  - `Lilac.exe`를 직접 실행하거나, `Lilac.exe <파일 경로>`로 특정 파일 지정 실행.
  - 인자 없이 실행 시 기본 안내 문서(`g_sampleMarkdown`) 표시.
- 파일 열기 방식:
  - 상단 메뉴 `파일(F)` -> `열기(O)...` (`Ctrl+O`)를 통한 파일 선택.
  - 실행 중인 창으로 `.md` 파일 드래그 앤 드롭 (`WM_DROPFILES`).
  - Windows 탐색기에서 `.md` 파일 더블 클릭 (파일 연결 등록 필요).

# 파일 연결
## 등록
- 마크다운(`.md`) 파일의 기본 실행 프로그램을 이 애플리케이션으로 설정.
- 상단 메뉴 `파일(F)` -> `.md 파일 기본 연결 등록(&R)` 선택 또는 명령줄 `Lilac.exe --register` 실행.
- `HKCU\Software\Classes\.md` 및 `HKCU\Software\Classes\Lilac.Document` 레지스트리에 등록되므로 관리자 권한(UAC) 불필요.
- 기본 아이콘(`DefaultIcon`)은 실행 파일의 0번 아이콘(`icon.ico`)으로 지정됨.

## 해제
- 등록된 파일 연결 정보 제거.
- 상단 메뉴 `파일(F)` -> `.md 파일 기본 연결 해제(&U)` 선택 또는 명령줄 `Lilac.exe --unregister` 실행.
- 등록되었던 레지스트리 키 삭제 후 셸 갱신(`SHChangeNotify`) 호출.

# 문서 내보내기
## PDF 내보내기
- 상단 메뉴 `파일(F)` -> `PDF로 내보내기(&P)...` (`Ctrl+P`) 선택 또는 명령줄 인자 사용.
- 명령줄 헤드리스 변환:
  - `Lilac.exe --export-pdf <입력_마크다운_경로> <출력_PDF_경로>`
  - 성공 시 `0`, 로드 실패 시 `2`, 변환 실패 시 `3` 반환.
- 특징:
  - 자체 TrueType 서브셋 폰트 엔진 내장 (`CIDFontType2` / `Type0`).
  - 제목(Heading 1~6)을 분석하여 PDF 북마크(Outlines) 자동 생성.
  - ToUnicode CMap 생성 (서로게이트 페어 이모지 지원, 텍스트 복사 및 검색 가능).
  - 하이퍼링크 및 앵커 점프를 PDF 링크 주석(`Link Annotation`)으로 처리.
  - 모노스페이스 글꼴에 대한 문자 간격 연산자(`Tc`) 자동 보정.

## Word (DOCX) 내보내기
- 상단 메뉴 `파일(F)` -> `DOCX로 내보내기(&D)...` (`Ctrl+D`) 선택 또는 명령줄 인자 사용.
- 명령줄 헤드리스 변환:
  - `Lilac.exe --export-docx <입력_마크다운_경로> <출력_DOCX_경로>`
  - 반환 코드는 PDF 내보내기와 동일.
- 특징:
  - Office Open XML(ECMA-376) 표준 형식의 ZIP 패키징 직접 구현.
  - 리스트 항목(`BK_LI`)의 첫 단락 텍스트와 불릿/체크박스 기호(`☑`, `☐`)를 동일 단락에 병합하여 불필요한 개행 방지.
  - 중첩 목록 레벨(0~8) 완벽 매핑.
  - GitHub 스타일 알림(Alerts) 박스에 좌측 테두리 및 배경 틴트 적용.
  - 코드 블록 내 탭(`\t`) 문자를 4칸 공백으로 치환하고 `xml:space="preserve"` 적용.
  - 표(Table) 상/하/좌/우 셀 패딩(`<w:tblCellMar>`) 및 헤더 행 볼드 서식 적용.

# 글꼴 및 대체(Fallback) 우선순위
## 글꼴 체인 구조
- 본문 글꼴(`FAM_BODY`)과 고정폭 글꼴(`FAM_MONO`)을 독립적으로 관리.
- 쉼표(`,`) 또는 세미콜론(`;`)으로 구분된 문자열로 글꼴 우선순위 체인 정의 (최대 16개).
- 앞선 글꼴에 없는 글리프는 뒤따르는 글꼴에서 순차적으로 탐색하여 렌더링.
- 이모지 및 기호 출력을 보장하기 위해 체인 끝에 `Segoe UI Emoji`, `Segoe UI Symbol`, `Malgun Gothic`이 자동으로 추가됨.

## 기본 체인
- 본문 체인: `Pretendard, 맑은 고딕, Segoe UI, Segoe UI Emoji, Segoe UI Symbol, Arial, Gulim`
- 고정폭 체인: `D2Coding, Consolas, 맑은 고딕, Segoe UI Emoji, Segoe UI Symbol, Courier New`

## 글꼴 설정 대화상자
- 상단 메뉴 `설정(S)` -> `글꼴 설정(&F)...` 선택.
- 본문 및 고정폭 글꼴의 체인 텍스트를 직접 편집하거나, `글꼴 추가...` 버튼으로 시스템 폰트를 골라 최우선 순위로 추가 가능.
- `기본값으로 초기화(&R)` 버튼으로 기본 체인으로 복원 가능.
- 설정값은 레지스트리 `HKCU\Software\Lilac\Settings`에 영구 저장되어 재실행 시 유지.

# 마크다운 지원 명세
- CommonMark 및 GitHub Flavored Markdown (GFM) 지원.
  - 제목 (`#` ~ `######`, 자동 앵커 생성)
  - 텍스트 강조 (굵게, 기울임꼴, 취소선, 밑줄, 인라인 코드, 하이라이트)
  - 글머리 기호 목록 (`*`, `-`, `+`) 및 번호 매기기 목록 (`1.`)
  - 작업 목록 체크박스 (`- [ ]`, `- [x]`)
  - GitHub 알림 블록 (`> [!NOTE]`, `> [!TIP]`, `> [!IMPORTANT]`, `> [!WARNING]`, `> [!CAUTION]`)
  - 인용구 (`>`)
  - 표 (`|`, 좌측/중앙/우측 정렬)
  - 코드 블록 (백틱 3개, 자간 오프셋 보정 및 탭 들여쓰기 유지)
  - 수평선 (`---`, `***`)
  - 이미지 (인라인/블록, PNG/JPEG/GIF/BMP 및 data URL 지원)
  - 각주 (`[^1]`)
  - 이모지 (BMP 기호 및 SMP 서로게이트 페어 이모지 지원)

# 단축키
- `Ctrl + O`: 파일 열기
- `Ctrl + P`: PDF로 내보내기
- `Ctrl + D` / `Ctrl + S`: Word (DOCX)로 내보내기
- `Ctrl + +` / `Ctrl + -`: 글꼴 크기 확대 / 축소
- `Ctrl + 0`: 글꼴 크기 기본값 복원
- `Ctrl + T`: 다크 모드 토글
- `F5`: 현재 문서 새로 고침

# 아이콘
- 리소스 ID: `IDI_APPICON` (`101`).
- `icon.ico` 파일을 프로젝트 루트에 두거나 `res/icon.ico`에 배치하면 빌드 시 `res/resource.rc`를 통해 리소스 섹션에 포함.
- 윈도우 클래스 등록(`WNDCLASSEXW.hIcon`, `hIconSm`) 및 창 생성 시 `WM_SETICON` 메시지를 통해 타이틀바와 작업표시줄에 적용.
- 파일 연결 등록 시 Windows 탐색기의 `.md` 파일 대표 아이콘으로 사용됨.

# 파일 구조
- `build.bat`: MSVC 64비트 정적 빌드 배치 스크립트.
- `src/`: C 소스 코드 및 헤더.
  - `main.c`: Win32 GUI 진입점, 메시지 루프, 파일 연결, 설정 입출력.
  - `doc.c` / `doc.h`: md4c 파서 래핑 및 AST 문서 모델 정의.
  - `layout.c` / `layout.h`: 텍스트 배치, 화면 뷰어 및 페이지 분할 레이아웃 계산.
  - `font.c` / `font.h`: 폰트 Fallback 체인 관리, TrueType CMap 파싱 및 글리프 탐색.
  - `pdf.c`: TrueType 서브셋 임베딩 기반 PDF 1.7 생성 엔진.
  - `docx.c`: OpenXML 기반 Word 문서 생성 엔진.
  - `export.h`: PDF/DOCX 내보내기 공통 인터페이스.
  - `image.c` / `image.h`: WIC 기반 이미지 로딩 및 PNG 인코딩.
  - `zip.c` / `zip.h`: 메모리 기반 ZIP 아카이브 생성기.
  - `util.c` / `util.h`: 아레나 메모리 풀, 가변 버퍼, UTF-8/16 변환, deflate 압축.
- `res/`: 윈도우 리소스.
  - `resource.h` / `resource.rc`: 메뉴, 단축키, 대화상자, 아이콘 정의.
  - `manifest.xml`: High-DPI(PerMonitorV2) 및 ComCtl32 v6 활성화 매니페스트.
  - `icon.ico`: 애플리케이션 기본 아이콘.
- `third_party/`:
  - `md4c/`: CommonMark / GFM 파서 C 라이브러리.
  - `sdefl/`: 경량 Deflate 압축 라이브러리.
