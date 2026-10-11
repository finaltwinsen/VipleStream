pragma Singleton
import QtQuick 2.9

// QML 測試用的替身：AppView.qml 只看 designVariant（Safe／Bold 兩種版面）
QtObject {
    enum DesignVariant { DV_SAFE, DV_BOLD }

    property int designVariant: 0
}
