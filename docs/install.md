# LiveContainer 설치 안내

unsigned arm64 IPA는 소유자의 빌드·패키지 검사 후 배포합니다. 아이폰 실기기 실행은 아직 확인하지 않았습니다. 아래 절차로 설치해 확인합니다.

1. 이 프로젝트의 `Build IPA`가 만든 암호화된 아티팩트를 소유자의 PC에서 `tools/release-ipa.ps1`로 복호화해 `dist/ipa/NFS4-<버전>-b<빌드>.ipa`를 얻습니다.
2. 파일 앱의 iCloud Drive › iOS 포트 IPA에서 최신 NFS4 IPA를 확인하고 LiveContainer의 + 버튼에서 선택합니다. 이전 버전은 PC의 버전별 보관함에 남깁니다. 앱의 기본(비공유) 컨테이너를 사용합니다.
3. 앱을 한 번 실행해 `LiveContainer › Games › NFS4 › game` 폴더를 만듭니다.
4. PC에서 이 프로젝트의 `tools/PC에서-받기-서버.cmd`를 실행합니다. 이미 같은 포트 8765의 서버가 실행 중이면 임의로 종료하지 말고 해당 서버 구성을 사용자와 확인합니다.
5. 같은 와이파이에서 앱의 `PC에서 받기`를 선택합니다. 목록의 모든 파일을 받은 뒤 실행합니다.
6. 문제가 생기면 앱 설정에 표시되는 버전·빌드 번호와 `LiveContainer › Games › NFS4 › save`의 `nfs4host.log`, `nfs4host.prev.log`를 확인합니다.

앱 ID: `io.github.avoseed.nfs4`. 앱 데이터 폴더: `NFS4`. 원본 게임 데이터는 IPA와 저장소에 포함하지 않습니다. 재컴파일 입력 exe/DLL 3개는 공개 저장소에서 암호화된 묶음으로만 보관하며, 키는 소유자의 PC와 저장소 Secret에만 둡니다.

LiveContainer 공식 설치·IPA 선택 안내: https://github.com/LiveContainer/LiveContainer#installing-apps
실기기에서 메뉴·레이스·사운드·조작·발열은 사용자가 직접 확인해야 하며, PC 검증만으로 아이폰 실행 성공을 확정하지 않습니다.

차량 텍스처 개선은 앱이 로딩할 때 적용하므로 기존 게임 데이터를 다시 받을 필요가 없습니다. RGB 경계의 계단 현상을 줄이고 원본 도색·유리 마스크를 유지합니다. 원본에 없는 새로운 디테일을 생성하는 기능은 아닙니다.
