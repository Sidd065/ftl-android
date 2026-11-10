# Sourced by build-android.sh: the Hyperspace it builds. One fork holds the iPad
# game's Hyperspace for every port (Android, Linux, Switch).
# REF is pinned to a commit so a build always gets the same Hyperspace; move it on
# deliberately. HS_REPO and HS_REF pick another clone, branch or commit (a commit
# by its full hash: git fetches no abbreviated ones).
REPO=${HS_REPO:-https://github.com/Sidd065/FTL-Hyperspace.git}
REF=${HS_REF:-11376777aa5d88ddfc74c0183d8e71cc062c6052}  # branch ipad-player-reports-android: PR #13 plus Multiverse's UI moves

# Checks out REF in SRC, fetching only that commit
hs_checkout() {
  [ -d "$SRC/.git" ] || git init -q "$SRC"
  git -C "$SRC" fetch -q --depth 1 "$REPO" "$REF"
  git -C "$SRC" checkout -q -f FETCH_HEAD
}
