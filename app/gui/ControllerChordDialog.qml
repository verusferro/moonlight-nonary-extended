import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.2

import StreamingPreferences 1.0

// Controller button chord picker. `preference` names the StreamingPreferences
// string property that holds the comma-separated SDL button indices.
NavigableDialog {
    id: dialog
    property string preference
    property bool allowEmpty: false
    standardButtons: Dialog.Ok | Dialog.Cancel

    function checkedCount() {
        var count = 0
        for (var i = 0; i < repeater.count; i++) {
            var item = repeater.itemAt(i)
            if (item && item.checked)
                count++
        }
        return count
    }

    function updateOkEnabled() {
        var count = checkedCount()
        if (dialog.standardButton)
            dialog.standardButton(Dialog.Ok).enabled = count >= 2 || (allowEmpty && count === 0)
    }

    function applyCurrentShortcut() {
        var selected = {}
        var parts = StreamingPreferences[preference].split(",")
        for (var i = 0; i < parts.length; i++) {
            if (parts[i].length > 0)
                selected[parts[i]] = true
        }
        for (var j = 0; j < repeater.count; j++) {
            var item = repeater.itemAt(j)
            if (item)
                item.checked = selected[String(item.buttonIndex)] === true
        }
        updateOkEnabled()
    }

    onOpened: {
        if (repeater.count > 0)
            applyCurrentShortcut()
        else
            Qt.callLater(applyCurrentShortcut)
    }

    onAccepted: {
        var values = []
        for (var i = 0; i < repeater.count; i++) {
            var item = repeater.itemAt(i)
            if (item && item.checked)
                values.push(String(item.buttonIndex))
        }
        if (values.length >= 2 || (allowEmpty && values.length === 0))
            StreamingPreferences[preference] = values.join(",")
    }

    ColumnLayout {
        spacing: 10

        Label {
            text: allowEmpty ? qsTr("Select at least two controller buttons, or none to disable.")
                             : qsTr("Select at least two controller buttons.")
            wrapMode: Text.Wrap
            Layout.maximumWidth: 480
        }

        GridLayout {
            columns: 3
            columnSpacing: 8
            rowSpacing: 4

            Repeater {
                id: repeater
                model: [
                    {i:0,n:"A / Cross"},{i:1,n:"B / Circle"},{i:2,n:"X / Square"},{i:3,n:"Y / Triangle"},
                    {i:4,n:"Share / Create"},{i:5,n:"Home / PS"},{i:6,n:"Start / Options"},
                    {i:7,n:"L3"},{i:8,n:"R3"},
                    {i:9,n:"LB"},{i:10,n:"RB"},{i:15,n:"Mute"},
                    {i:16,n:"Paddle 1"},{i:17,n:"Paddle 2"},{i:18,n:"Paddle 3"},{i:19,n:"Paddle 4"},
                    {i:20,n:"Touchpad"}
                ]
                CheckBox {
                    property int buttonIndex: modelData.i
                    text: modelData.n
                    onToggled: dialog.updateOkEnabled()
                }
            }
        }
    }
}
