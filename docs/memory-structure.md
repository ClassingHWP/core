# 메모리 구조

`x-store`가 `value`면 그 객체가 값을 가진다. `ref`면 번호나 범위만 가진다. 화면 캔버스는 하나다.

## Document

```json
{
  "Document": {
    "type": "object",
    "properties": {
      "charShapes": {
        "type": "array",
        "x-store": "value",
        "items": {
          "height": "글자 크기. 1000 = 10pt",
          "fontRef": "언어별 글꼴 번호",
          "ratio": "장평 %",
          "spacing": "자간 %",
          "relSize": "상대 크기 %",
          "offset": "글자 위치 %",
          "textColor": "글자색",
          "shadeColor": "음영",
          "bold": "굵게",
          "italic": "기울임",
          "underline": "NONE | BOTTOM | TOP",
          "underlineShape": "밑줄 모양",
          "underlineColor": "밑줄 색",
          "strikeout": "취소선",
          "strikeShape": "취소선 모양",
          "strikeColor": "취소선 색",
          "outline": "외곽선",
          "shadow": "그림자",
          "emphasis": "강조점",
          "superscript": "위첨자",
          "subscript": "아래첨자",
          "borderFillId": { "$ref": "#/BorderFill", "x-store": "ref" }
        }
      },
      "paraShapes": {
        "type": "array",
        "x-store": "value",
        "items": {
          "align": "JUSTIFY | LEFT | RIGHT | CENTER | DISTRIBUTE",
          "vertAlign": "BASELINE | TOP | CENTER | BOTTOM",
          "indent": "첫 줄. 음수는 내어쓰기",
          "marginLeft": "왼쪽 여백",
          "marginRight": "오른쪽 여백",
          "spacingPrev": "문단 위 간격",
          "spacingNext": "문단 아래 간격",
          "lineSpacingType": "PERCENT | FIXED | BETWEEN_LINES | AT_LEAST",
          "lineSpacing": "줄 간격 값",
          "borderFillId": { "$ref": "#/BorderFill", "x-store": "ref" },
          "borderOffsetLeft": "테두리 간격",
          "borderConnect": "테두리 연결",
          "borderIgnoreMargin": "여백 무시"
        }
      },
      "styles": {
        "type": "array",
        "x-store": "value",
        "items": {
          "name": "스타일 이름",
          "paraShapeId": { "$ref": "#/ParaShape", "x-store": "ref" },
          "charShapeId": { "$ref": "#/CharShape", "x-store": "ref" },
          "nextStyleId": { "$ref": "#/Style", "x-store": "ref" }
        }
      },
      "borderFills": {
        "type": "array",
        "x-store": "value",
        "items": {
          "left": "BorderSide",
          "right": "BorderSide",
          "top": "BorderSide",
          "bottom": "BorderSide",
          "faceColor": "배경. none 이면 없음",
          "gradColor": "그러데이션 끝 색",
          "gradAngle": "0 아래→위, 90 왼→오",
          "imageId": { "$ref": "#/BinItem", "x-store": "ref" }
        }
      },
      "binItems": {
        "type": "array",
        "x-store": "value",
        "description": "그림 파일 바이트"
      },
      "sections": {
        "type": "array",
        "x-store": "value",
        "items": { "$ref": "#/Section" }
      }
    }
  },
  "Section": {
    "type": "object",
    "properties": {
      "page": {
        "x-store": "value",
        "width": "쪽 너비",
        "height": "쪽 높이",
        "landscape": "가로",
        "marginLeft": "왼쪽",
        "marginRight": "오른쪽",
        "marginTop": "위",
        "marginBottom": "아래",
        "marginHeader": "머리말 여백",
        "marginFooter": "꼬리말 여백",
        "marginGutter": "제본"
      },
      "defaultTab": { "x-store": "value", "description": "탭 정의가 없을 때의 간격" },
      "columnRegionGap": { "x-store": "value", "description": "같은 쪽에서 단 설정이 바뀔 때 간격. 기본 4mm" },
      "pageStart": { "x-store": "value", "description": "0이면 앞 구역 다음 번호" },
      "hideHeader": { "x-store": "value" },
      "hideFooter": { "x-store": "value" },
      "hideMaster": { "x-store": "value" },
      "hidePageNum": { "x-store": "value" },
      "hideFirstHeader": { "x-store": "value" },
      "hideFirstFooter": { "x-store": "value" },
      "hideFirstMaster": { "x-store": "value" },
      "hideFirstPageNum": { "x-store": "value" },
      "masterPages": { "x-store": "value", "description": "바탕쪽 문단" },
      "footnote": { "x-store": "value", "description": "각주 모양" },
      "endnote": { "x-store": "value", "description": "미주 모양" },
      "pageBorders": { "x-store": "value", "description": "BOTH | EVEN | ODD 쪽 테두리·배경" },
      "paragraphs": {
        "type": "array",
        "x-store": "value",
        "items": { "$ref": "#/Paragraph" }
      }
    }
  },
  "Paragraph": {
    "type": "object",
    "properties": {
      "paraShapeId": { "$ref": "#/ParaShape", "x-store": "ref" },
      "styleId": { "$ref": "#/Style", "x-store": "ref" },
      "pageBreak": { "x-store": "value" },
      "columnBreak": { "x-store": "value" },
      "layoutBreak": { "x-store": "value" },
      "runs": {
        "type": "array",
        "x-store": "value",
        "items": {
          "charShapeId": { "$ref": "#/CharShape", "x-store": "ref" },
          "items": {
            "type": "array",
            "x-store": "value",
            "items": {
              "text": { "x-store": "value" },
              "markpen": { "x-store": "value", "description": "형광펜. CharShape 와 따로" },
              "control": { "$ref": "#/Control", "x-store": "value" }
            }
          }
        }
      },
      "lineSegs": {
        "type": "array",
        "x-store": "value",
        "description": "저장용. textPos, vertPos, horzPos, horzSize, textHeight. vertPos·horzPos 는 단 안 좌표"
      },
      "ranges": { "x-store": "value", "description": "영역 태그. 종류 2는 형광펜" }
    }
  },
  "Control": {
    "type": "object",
    "description": "문단 글자 흐름 안의 앵커",
    "properties": {
      "kind": "SectionDef | ColumnDef | Table | Picture | Shape | Header | Footer | ...",
      "placement": {
        "x-store": "value",
        "treatAsChar": "글자처럼 취급",
        "vertRelTo": "Paper | Page | Para",
        "horzRelTo": "Paper | Page | Column | Para",
        "vertAlign": "TOP | CENTER | BOTTOM",
        "horzAlign": "LEFT | CENTER | RIGHT",
        "vertOffset": "HWPUNIT",
        "horzOffset": "HWPUNIT",
        "width": "현재 너비",
        "height": "현재 높이",
        "textWrap": "SQUARE | TOP_AND_BOTTOM | BEHIND_TEXT | IN_FRONT_OF_TEXT",
        "marginLeft": "바깥 여백",
        "zOrder": "쌓임"
      },
      "columnDef": {
        "x-store": "value",
        "description": "단 정보는 구역 객체가 아니라 이 컨트롤에 있다. 문단 흐름 안 cold",
        "columnCount": "단 수",
        "columnGap": "같은 너비일 때 단 사이 간격",
        "columnSameSize": "단 너비가 같은지",
        "columnWidths": [{ "width": "단 너비", "gap": "오른쪽 간격" }],
        "columnType": "NEWSPAPER | PARALLEL | NORMAL",
        "columnLayout": "LEFT | RIGHT | BOTH",
        "columnLineType": "단 구분선. 비거나 NONE 이면 없음",
        "columnLineWidth": "HWPUNIT",
        "columnLineColor": "색",
        "columnRegionGap": "단 설정이 바뀔 때 간격"
      },
      "sectionDef": {
        "x-store": "value",
        "description": "쪽 정의가 바뀌는 값. 문단 흐름 안 secd. 적용 결과는 Section.page 에 모인다",
        "page": { "$ref": "#/Section/properties/page" }
      },
      "picture": { "x-store": "value", "description": "clip, originWidth, rectWidth, bright, contrast, effect" },
      "binItemId": { "$ref": "#/BinItem", "x-store": "ref" },
      "subLists": {
        "x-store": "value",
        "description": "표 칸, 글상자, 머리말, 꼬리말, 캡션, 각주. 안쪽은 Paragraph"
      }
    }
  }
}
```

단 개수·간격·너비·구분선은 `Section` 필드가 아니다. 그 구역 `Paragraph.runs` 안의 `ColumnDef`다. 구역이 직접 가진 단 값은 `columnRegionGap`뿐이다.

## Layout

```json
{
  "LayoutSection": {
    "type": "object",
    "properties": {
      "section": { "$ref": "#/Section", "x-store": "ref" },
      "firstPage": { "x-store": "value" },
      "lastPage": { "x-store": "value" },
      "papers": { "type": "array", "items": { "$ref": "#/Paper" }, "x-store": "value" }
    }
  },
  "Paper": {
    "type": "object",
    "properties": {
      "section": { "$ref": "#/Section", "x-store": "ref" },
      "pageNumber": { "x-store": "value", "description": "구역 안 번호. 홀짝" },
      "width": { "x-store": "value", "description": "PageDef 로 계산한 결과" },
      "height": { "x-store": "value" },
      "marginLeft": { "x-store": "value" },
      "marginRight": { "x-store": "value" },
      "marginTop": { "x-store": "value" },
      "marginBottom": { "x-store": "value" },
      "marginHeader": { "x-store": "value" },
      "marginFooter": { "x-store": "value" },
      "header": { "$ref": "#/Band", "x-store": "value" },
      "body": { "$ref": "#/Flow", "x-store": "value" },
      "footer": { "$ref": "#/Band", "x-store": "value" }
    }
  },
  "Band": {
    "description": "Header 또는 Footer. 문단을 복사하지 않는다",
    "properties": {
      "control": { "$ref": "#/Control", "x-store": "ref", "description": "이 쪽 번호의 Header 또는 Footer. 숨기면 null" },
      "columns": { "type": "array", "items": { "$ref": "#/Column" }, "x-store": "value" }
    }
  },
  "Flow": {
    "description": "Body",
    "properties": {
      "columns": { "type": "array", "items": { "$ref": "#/Column" }, "x-store": "value" }
    }
  },
  "Column": {
    "type": "object",
    "properties": {
      "def": { "$ref": "#/Control/properties/columnDef", "x-store": "ref" },
      "index": { "x-store": "value" },
      "x": { "x-store": "value", "description": "쪽 기준" },
      "y": { "x-store": "value" },
      "width": { "x-store": "value", "description": "columnWidths 또는 균등 분할 결과" },
      "height": { "x-store": "value" },
      "gap": { "x-store": "value", "description": "오른쪽 간격. 마지막 단은 0" },
      "paragraphs": { "type": "array", "items": { "$ref": "#/LayoutPara" }, "x-store": "value" }
    }
  },
  "LayoutPara": {
    "type": "object",
    "properties": {
      "paragraph": { "$ref": "#/Paragraph", "x-store": "ref" },
      "from": { "x-store": "value", "description": "이 단에 놓인 내용 시작" },
      "to": { "x-store": "value", "description": "이 단에 놓인 내용 끝" },
      "x": { "x-store": "value" },
      "y": { "x-store": "value" },
      "width": { "x-store": "value" },
      "height": { "x-store": "value" },
      "lines": { "type": "array", "items": { "$ref": "#/Line" }, "x-store": "value" },
      "floats": {
        "type": "array",
        "x-store": "value",
        "description": "글자처럼 취하지 않은 개체. 부모는 이 문단",
        "items": {
          "control": { "$ref": "#/Control", "x-store": "ref" },
          "anchor": { "x-store": "value", "description": "문단 내용 오프셋" },
          "x": { "x-store": "value", "description": "단 또는 쪽 기준 계산 결과" },
          "y": { "x-store": "value" },
          "width": { "x-store": "value" },
          "height": { "x-store": "value" }
        }
      }
    }
  },
  "Line": {
    "type": "object",
    "properties": {
      "from": { "x-store": "value" },
      "to": { "x-store": "value" },
      "x": { "x-store": "value", "description": "정렬·들여쓰기 반영" },
      "y": { "x-store": "value", "description": "쪽 좌표" },
      "width": { "x-store": "value", "description": "단 너비가 아니라 이 줄의 너비" },
      "height": { "x-store": "value" },
      "textHeight": { "x-store": "value" },
      "baseline": { "x-store": "value" },
      "spans": {
        "type": "array",
        "x-store": "value",
        "items": {
          "charShapeId": { "$ref": "#/CharShape", "x-store": "ref" },
          "from": { "x-store": "value" },
          "to": { "x-store": "value" }
        }
      },
      "inlineObjects": {
        "type": "array",
        "x-store": "value",
        "description": "treatAsChar. 줄 높이에 포함",
        "items": {
          "control": { "$ref": "#/Control", "x-store": "ref" },
          "anchor": { "x-store": "value" }
        }
      }
    }
  }
}
```

## Render

```json
{
  "Render": {
    "type": "object",
    "properties": {
      "glyphAtlas": {
        "x-store": "value",
        "description": "글꼴·크기·굵기·글자 번호마다 문서에서 하나인 픽셀"
      },
      "pictureTextures": {
        "x-store": "value",
        "key": ["binItemId", "sizeLevel"],
        "clip": { "$ref": "#/Control/properties/picture", "x-store": "ref" }
      },
      "nodeCache": {
        "x-store": "value",
        "description": "Layout 노드 번호 → 배경·테두리 획·밑줄·취소선·형광펜 사각형. 색은 CharShape·BorderFill·markpen 을 읽는다"
      },
      "overview": {
        "x-store": "value",
        "description": "Paper 마다 하나. 축소해서 쪽이 많을 때만 사용. Layout 이 바뀌면 그 쪽만 버린다"
      }
    }
  }
}
```

화면은 쪽 사각형으로 보이는 `Paper`를 고르고, 그 안 노드를 Y로 이진 탐색한 뒤 캔버스 하나에 `translate`와 `scale`을 넣어 `nodeCache`와 글자 쿼드, 그림 텍스처를 그린다. 확대·축소 중에는 이 JSON을 다시 만들지 않는다.

## 저장

Layout `Line`을 그 Document `Paragraph.lineSegs`로 모은다. 여러 쪽·여러 단에 나뉘어도 기록은 그 문단 하나다. `textPos`는 내용 시작이고, `vertPos`와 `horzPos`는 단 안 위치다. `ColumnDef`와 `placement`는 Document `Control` 그대로 적는다.
