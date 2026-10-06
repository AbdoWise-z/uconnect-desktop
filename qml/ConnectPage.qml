import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// The "not connected" page, laid out like the JetBrains welcome screen.
Rectangle {
    id: page

    readonly property bool connecting: App.status === "connecting"
    readonly property bool canConnect: App.topicLinkValid && App.isValidServer(App.serverAddress) && !connecting
    property bool justCreated: false
    property bool revealLink: false

    signal settingsRequested(string pageId)

    color: Theme.canvas

    component NavItem: Rectangle {
        id: nav
        property alias text: navLabel.text
        property bool selected: false
        signal clicked()

        Layout.fillWidth: true
        implicitHeight: 30
        radius: Theme.radius
        color: selected ? Theme.selection : navArea.containsMouse ? Theme.hover : "transparent"

        Text {
            id: navLabel
            anchors.verticalCenter: parent.verticalCenter
            x: 10
            color: Theme.text
        }
        MouseArea {
            id: navArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: nav.clicked()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- sidebar -------------------------------------------------------
        Rectangle {
            Layout.preferredWidth: 230
            Layout.fillHeight: true
            color: Theme.panel

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 4

                RowLayout {
                    Layout.margins: 6
                    Layout.bottomMargin: 18
                    spacing: 10
                    Avatar {
                        name: "u Connect"
                        seed: "uConnect"
                        size: 32
                    }
                    ColumnLayout {
                        spacing: 0
                        Text {
                            text: "uConnect"
                            color: Theme.text
                            font.bold: true
                        }
                        Text {
                            text: Qt.application.version
                            color: Theme.textSecondary
                            font.pixelSize: Theme.smallFontSize
                        }
                    }
                }

                NavItem {
                    text: qsTr("Topics")
                    selected: true
                }

                Item { Layout.fillHeight: true }

                NavItem {
                    text: qsTr("Settings")
                    onClicked: page.settingsRequested("")
                }
            }
        }

        Rectangle {
            Layout.fillHeight: true
            implicitWidth: 1
            color: Theme.divider
        }

        // --- content -------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.top: parent.top
                anchors.topMargin: 72
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(580, parent.width - 80)
                spacing: 10

                Text {
                    text: qsTr("Connect to a Topic")
                    color: Theme.text
                    font.pixelSize: 20
                }
                Text {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 14
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                    text: qsTr("Devices that join the same protected topic can find and connect to each other. Paste an invite link from one of your devices, or start a new topic.")
                }

                Text {
                    text: qsTr("Invite link")
                    color: Theme.text
                }
                RowLayout {
                    spacing: 6
                    UTextField {
                        id: linkField
                        Layout.fillWidth: true
                        text: App.topicLink
                        placeholderText: "uconn://..."
                        echoMode: page.revealLink ? TextInput.Normal : TextInput.Password
                        enabled: !page.connecting
                        invalid: text.length > 0 && !App.topicLinkValid
                        onTextEdited: {
                            App.topicLink = text
                            page.justCreated = false
                        }
                        onAccepted: if (page.canConnect) App.connectToTopic()
                    }
                    UToolButton {
                        implicitHeight: Theme.controlHeight
                        text: page.revealLink ? qsTr("Hide") : qsTr("Show")
                        tip: qsTr("Show or hide the invite link")
                        onClicked: page.revealLink = !page.revealLink
                    }
                }
                Text {
                    visible: linkField.invalid
                    font.pixelSize: Theme.smallFontSize
                    color: Theme.error
                    text: qsTr("Not a protected uconn:// link. Open topics (without a key) are not accepted.")
                }

                RowLayout {
                    Layout.topMargin: 6
                    spacing: 8
                    UButton {
                        visible: !page.connecting
                        text: qsTr("Connect")
                        primary: true
                        enabled: page.canConnect
                        onClicked: App.connectToTopic()
                    }
                    UButton {
                        visible: page.connecting
                        text: qsTr("Cancel")
                        onClicked: App.disconnectFromTopic()
                    }
                    UButton {
                        text: qsTr("New Topic")
                        enabled: !page.connecting
                        onClicked: {
                            App.generateTopicLink()
                            page.justCreated = true
                        }
                    }
                    UButton {
                        text: qsTr("Paste Link")
                        enabled: !page.connecting
                        onClicked: {
                            App.pasteTopicLink()
                            page.justCreated = false
                        }
                    }
                }

                // Indeterminate progress while connecting.
                ColumnLayout {
                    visible: page.connecting
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    spacing: 6

                    Rectangle {
                        id: track
                        Layout.fillWidth: true
                        implicitHeight: 3
                        radius: 2
                        color: Theme.border
                        clip: true

                        Rectangle {
                            id: chunk
                            width: track.width * 0.3
                            height: parent.height
                            radius: 2
                            color: Theme.accent
                            NumberAnimation on x {
                                running: page.connecting
                                loops: Animation.Infinite
                                from: -chunk.width
                                to: track.width
                                duration: 1200
                            }
                        }
                    }
                    Text {
                        text: App.statusDetail
                        color: Theme.textSecondary
                        font.pixelSize: Theme.smallFontSize
                    }
                }

                Banner {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: App.status === "error"
                    kind: "error"
                    text: App.statusDetail
                }

                Banner {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: page.justCreated && App.status !== "error"
                    text: qsTr("New topic created. Send its invite link to your other devices; it contains the topic key, so share it only with devices you own.")
                    UButton {
                        text: qsTr("Copy Link")
                        onClicked: App.copyTopicLink()
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 24
                    implicitHeight: 1
                    color: Theme.border
                }

                RowLayout {
                    spacing: 6
                    Text {
                        text: qsTr("Rendezvous server:")
                        color: Theme.textSecondary
                    }
                    Text {
                        text: App.serverAddress.length > 0 ? App.serverAddress : qsTr("not set")
                        color: Theme.text
                    }
                    ULink {
                        text: qsTr("Change...")
                        onClicked: page.settingsRequested("connection")
                    }
                }
            }
        }
    }
}
