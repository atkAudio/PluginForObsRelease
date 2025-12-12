#pragma once

#include <QWidget>
#include <QDockWidget>
#include <QMainWindow>
#include <QResizeEvent>
#include <QPainter>
#include <QWindow>
#include <QGuiApplication>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <obs-module.h>

namespace atk
{

// Qt widget that hosts a JUCE Component.
// On Windows/macOS/X11: embeds the JUCE component directly.
// On Wayland: shows a separate JUCE DocumentWindow as fallback.
class JuceQtWidget
    : public QWidget
    , private juce::ComponentListener
{
public:
    using OnShowCallback = std::function<void()>;
    using OnHideCallback = std::function<void()>;
    using OnDockStateChangedCallback = std::function<void(bool isDocked)>;

    explicit JuceQtWidget(
        juce::Component* juceComponent,
        OnShowCallback onShowCb = nullptr,
        OnHideCallback onHideCb = nullptr,
        QWidget* parent = nullptr
    )
        : QWidget(parent)
        , component(juceComponent)
        , onShow(std::move(onShowCb))
        , onHide(std::move(onHideCb))
    {
        useWaylandMode = (QGuiApplication::platformName() == "wayland");

        if (useWaylandMode)
        {
            setMinimumSize(200, 100);
        }
        else
        {
            setAttribute(Qt::WA_NativeWindow, true);
            setAttribute(Qt::WA_DontCreateNativeAncestors, false);
            setAttribute(Qt::WA_OpaquePaintEvent);
            setAttribute(Qt::WA_NoSystemBackground);
            setMinimumSize(50, 50);
        }

        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);

        if (component)
            component->addComponentListener(this);
    }

    ~JuceQtWidget() override
    {
        if (connectedDock)
        {
            disconnect(connectedDock, nullptr, this, nullptr);
            connectedDock = nullptr;
        }

        onShow = nullptr;
        onHide = nullptr;
        onDockStateChanged = nullptr;
        getConstrainer = nullptr;

        if (juceWindow)
        {
            juceWindow->setVisible(false);
            juceWindow = nullptr;
        }

        if (!component)
            return;

        component->removeComponentListener(this);

        auto* comp = component;
        component = nullptr;

        auto lambda = [comp]()
        {
            if (comp->isOnDesktop())
                comp->removeFromDesktop();
            delete comp;
        };

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            lambda();
        else
            juce::MessageManager::callAsync(lambda);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);

        if (!component || resizingFromJuce)
            return;

        if (!useWaylandMode)
        {
            updateJuceComponentBounds();
            if (component->isOnDesktop())
                component->repaint();
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        if (useWaylandMode)
        {
            QPainter painter(this);
            painter.fillRect(rect(), QColor(39, 42, 51));
            painter.setPen(QColor(200, 200, 200));
            painter.drawText(rect(), Qt::AlignCenter, "Plugin window is shown separately\n(Wayland mode)");
        }
    }

    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        connectToParentDock();

        if (!useWaylandMode)
            (void)winId();

        QMetaObject::invokeMethod(this, [this]() { performDeferredShow(); }, Qt::QueuedConnection);
    }

    void hideEvent(QHideEvent* event) override
    {
        QWidget::hideEvent(event);

        if (useWaylandMode)
        {
            if (juceWindow)
                juceWindow->setVisible(false);
        }
        else
        {
            if (component && component->isOnDesktop())
                component->removeFromDesktop();
        }

        if (onHide)
            onHide();
    }

    juce::Component* getJuceComponent() const
    {
        return component;
    }

    void setConstrainerGetter(std::function<juce::ComponentBoundsConstrainer*()> getter)
    {
        getConstrainer = std::move(getter);
    }

    void setDockStateChangedCallback(OnDockStateChangedCallback callback)
    {
        onDockStateChanged = std::move(callback);
    }

    void clearCallbacks()
    {
        onShow = nullptr;
        onHide = nullptr;
        onDockStateChanged = nullptr;
        getConstrainer = nullptr;
    }

    QSize sizeHint() const override
    {
        if (useWaylandMode)
            return QSize(200, 100);
        if (component)
            return QSize(component->getWidth(), component->getHeight());
        return QSize(500, 500);
    }

    bool isDocked() const
    {
        if (connectedDock)
            return !connectedDock->isFloating();
        return false;
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::ScreenChangeInternal)
            resetConstraints();

        if (event->type() == QEvent::ParentChange)
            connectToParentDock();

        return QWidget::event(event);
    }

private:
    // JUCE DocumentWindow for Wayland mode
    class WaylandWindow : public juce::DocumentWindow
    {
    public:
        WaylandWindow(juce::Component* content, std::function<void()> onCloseCb)
            : juce::DocumentWindow("atkAudio PluginHost", juce::Colour(39, 42, 51), juce::DocumentWindow::allButtons)
            , onClose(std::move(onCloseCb))
        {
            setUsingNativeTitleBar(true);
            setContentNonOwned(content, true);
            setResizable(true, false);
            centreWithSize(content->getWidth(), content->getHeight());
        }

        void closeButtonPressed() override
        {
            setVisible(false);
            if (onClose)
                onClose();
        }

    private:
        std::function<void()> onClose;
    };

    void connectToParentDock()
    {
        QDockWidget* parentDock = nullptr;
        QWidget* p = parentWidget();
        while (p)
        {
            if (QDockWidget* dock = qobject_cast<QDockWidget*>(p))
            {
                parentDock = dock;
                break;
            }
            p = p->parentWidget();
        }

        if (parentDock == connectedDock)
            return;

        if (connectedDock)
            disconnect(connectedDock, nullptr, this, nullptr);

        connectedDock = parentDock;
        if (connectedDock)
            connect(connectedDock, &QDockWidget::topLevelChanged, this, &JuceQtWidget::onTopLevelChanged);
    }

    void onTopLevelChanged(bool floating)
    {
        if (onDockStateChanged)
            onDockStateChanged(!floating);
    }

    void performDeferredShow()
    {
        resetConstraints();

        notifyDockState();

        if (!component)
            return;

        if (useWaylandMode)
            setupWaylandWindow();
        else
            embedJuceComponent();

        if (onShow)
            onShow();

        component->setVisible(true);
        component->repaint();
    }

    void resetConstraints()
    {
        // Reset to permissive defaults before applying new constraints
        setMinimumSize(50, 50);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

        if (QWidget* parentDock = parentWidget())
        {
            parentDock->setMinimumSize(50, 50);
            parentDock->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
    }

    void setupWaylandWindow()
    {
        if (!juceWindow)
            juceWindow = std::make_unique<WaylandWindow>(component, nullptr);

        juceWindow->setVisible(true);
        juceWindow->toFront(true);
    }

    void embedJuceComponent()
    {
        if (!component)
            return;

        QWindow* qtWindow = windowHandle();
        if (!qtWindow)
        {
            create();
            qtWindow = windowHandle();
        }

        if (!qtWindow)
            return;

        WId nativeHandle = qtWindow->winId();
        if (nativeHandle == 0)
            return;

        void* parentHandle = reinterpret_cast<void*>(nativeHandle);

        if (component->isOnDesktop())
            component->removeFromDesktop();

        component->addToDesktop(0, parentHandle);

        component->setVisible(true);
        component->toFront(false);
        component->repaint();
    }

    void componentMovedOrResized(juce::Component& comp, bool wasMoved, bool wasResized) override
    {
        if (&comp != component || !wasResized || resizingFromQt || useWaylandMode)
            return;

        resizingFromJuce = true;

        int newW = component->getWidth();
        int newH = component->getHeight();

        if (newW > 0 && newH > 0)
        {
            resetConstraints();

            if (newW != width() || newH != height())
            {
                resize(newW, newH);

                if (QWidget* parentDock = parentWidget())
                    parentDock->resize(newW, newH);
            }
        }

        resizingFromJuce = false;
    }

    void notifyDockState()
    {
        if (connectedDock && onDockStateChanged)
            onDockStateChanged(isDocked());
    }

    void updateJuceComponentBounds()
    {
        if (!component)
            return;

        int requestedW = width();
        int requestedH = height();

        if (requestedW <= 0 || requestedH <= 0)
            return;

        resizingFromQt = true;
        component->setBounds(0, 0, requestedW, requestedH);
        resizingFromQt = false;

        int actualW = component->getWidth();
        int actualH = component->getHeight();

        if (actualW > requestedW)
            setMinimumWidth(actualW);
        if (actualH > requestedH)
            setMinimumHeight(actualH);
        if (actualW < requestedW)
            setMaximumWidth(actualW);
        if (actualH < requestedH)
            setMaximumHeight(actualH);

        if (QWidget* parentDock = parentWidget())
        {
            parentDock->setMinimumSize(minimumSize());
            parentDock->setMaximumSize(maximumSize());
        }
    }

    juce::Component* component = nullptr;
    OnShowCallback onShow;
    OnHideCallback onHide;
    OnDockStateChangedCallback onDockStateChanged;
    std::function<juce::ComponentBoundsConstrainer*()> getConstrainer;
    QDockWidget* connectedDock = nullptr;
    bool resizingFromJuce = false;
    bool resizingFromQt = false;

    // Wayland-specific members
    bool useWaylandMode = false;
    std::unique_ptr<WaylandWindow> juceWindow;
};

} // namespace atk
